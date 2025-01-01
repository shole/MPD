// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "config.h"
#include "Filter.hxx"
#include "NotSongFilter.hxx"
#include "UriSongFilter.hxx"
#include "BaseSongFilter.hxx"
#include "TagSongFilter.hxx"
#include "ModifiedSinceSongFilter.hxx"
#include "AddedSinceSongFilter.hxx"
#include "AudioFormatSongFilter.hxx"
#include "PrioritySongFilter.hxx"
#include "pcm/AudioParser.hxx"
#include "tag/ParseName.hxx"
#include "tag/Type.hxx"
#include "time/ISO8601.hxx"
#include "lib/fmt/RuntimeError.hxx"
#include "util/CharUtil.hxx"
#include "util/StringCompare.hxx"
#include "util/StringStrip.hxx"
#include "util/ASCII.hxx"
#include "util/UriUtil.hxx"

#include <cassert>

#include <stdlib.h>

#define LOCATE_TAG_FILE_KEY     "file"
#define LOCATE_TAG_FILE_KEY_OLD "filename"
#define LOCATE_TAG_ANY_KEY      "any"

enum {
	/**
	 * Limit the search to files within the given directory.
	 */
	LOCATE_TAG_BASE_TYPE = TAG_NUM_OF_ITEM_TYPES + 1,

	LOCATE_TAG_MODIFIED_SINCE,
	LOCATE_TAG_AUDIO_FORMAT,
	LOCATE_TAG_PRIORITY,
	LOCATE_TAG_FILE_TYPE,
	LOCATE_TAG_ANY_TYPE,
	LOCATE_TAG_ADDED_SINCE,
};

/**
 * @return #TAG_NUM_OF_ITEM_TYPES on error
 */
[[gnu::pure]]
static unsigned
locate_parse_type(const char *str) noexcept
{
	if (StringEqualsCaseASCII(str, LOCATE_TAG_FILE_KEY) ||
	    StringEqualsCaseASCII(str, LOCATE_TAG_FILE_KEY_OLD))
		return LOCATE_TAG_FILE_TYPE;

	if (StringEqualsCaseASCII(str, LOCATE_TAG_ANY_KEY))
		return LOCATE_TAG_ANY_TYPE;

	if (strcmp(str, "base") == 0)
		return LOCATE_TAG_BASE_TYPE;

	if (strcmp(str, "modified-since") == 0)
		return LOCATE_TAG_MODIFIED_SINCE;

	if (strcmp(str, "added-since") == 0)
		return LOCATE_TAG_ADDED_SINCE;

	if (StringEqualsCaseASCII(str, "AudioFormat"))
		return LOCATE_TAG_AUDIO_FORMAT;

	if (StringEqualsCaseASCII(str, "prio"))
		return LOCATE_TAG_PRIORITY;

	return tag_name_parse_i(str);
}

SongFilter::SongFilter(TagType tag, const char *value, bool fold_case)
{
	/* for compatibility with MPD 0.20 and older, "fold_case" also
	   switches on "substring" */
	const auto position = fold_case
		? StringFilter::Position::ANYWHERE
		: StringFilter::Position::FULL;

	and_filter.AddItem(std::make_unique<TagSongFilter>(tag,
							   StringFilter(value, fold_case, position, false)));
}

/* this destructor exists here just so it won't get inlined */
SongFilter::~SongFilter() = default;

std::string
SongFilter::ToExpression() const noexcept
{
	return and_filter.ToExpression();
}

static std::chrono::system_clock::time_point
ParseTimeStamp(const char *s)
{
	assert(s != nullptr);

	try {
		/* try ISO 8601 */
		return ParseISO8601(s).first;
	} catch (...) {
		char *endptr;
		unsigned long long value = strtoull(s, &endptr, 10);
		if (*endptr == 0 && endptr > s)
			/* it's an integral UNIX time stamp */
			return std::chrono::system_clock::from_time_t((time_t)value);

		/* rethrow the ParseISO8601() error */
		throw;
	}
}

static constexpr bool
IsTagNameChar(char ch) noexcept
{
	return IsAlphaASCII(ch) || ch == '_' || ch == '-';
}

static const char *
FirstNonTagNameChar(const char *s) noexcept
{
	while (IsTagNameChar(*s))
		++s;
	return s;
}

static auto
ExpectWord(const char *&s)
{
	const char *begin = s;
	const char *end = FirstNonTagNameChar(s);
	if (end == s)
		throw std::runtime_error("Word expected");

	s = StripLeft(end);
	return std::string(begin, end);
}

static auto
ExpectFilterType(const char *&s)
{
	const auto name = ExpectWord(s);

	const auto type = locate_parse_type(name.c_str());
	if (type == TAG_NUM_OF_ITEM_TYPES)
		throw FmtRuntimeError("Unknown filter type: {}", name);

	return type;
}

static constexpr bool
IsQuote(char ch) noexcept
{
	return ch == '"' || ch == '\'';
}

static std::string
ExpectQuoted(const char *&s)
{
	const char quote = *s++;
	if (!IsQuote(quote))
		throw std::runtime_error("Quoted string expected");

	char buffer[4096];
	size_t length = 0;

	while (*s != quote) {
		if (*s == '\\')
			/* backslash escapes the following character */
			++s;

		if (*s == 0)
			throw std::runtime_error("Closing quote not found");

		buffer[length++] = *s++;

		if (length >= sizeof(buffer))
			throw std::runtime_error("Quoted value is too long");
	}

	s = StripLeft(s + 1);

	return {buffer, length};
}

/**
 * Operator definition used to parse the operator
 * from the command and create the StringFilter
 * if it matched the operator prefix.
 */
struct OperatorDef {
	const char *prefix;
	bool fold_case;
	bool negated;
	StringFilter::Position position;
};

/**
 * Pre-defined operators with explicit case-sensitivity.
 */
static constexpr std::array<OperatorDef, 12> operators = {
	//            operator prefix     fold case  negated     position
	OperatorDef { "contains_cs ",     false,     false,      StringFilter::Position::ANYWHERE },
	OperatorDef { "!contains_cs ",    false,     true,       StringFilter::Position::ANYWHERE },
	OperatorDef { "contains_ci ",     true,      false,      StringFilter::Position::ANYWHERE },
	OperatorDef { "!contains_ci ",    true,      true,       StringFilter::Position::ANYWHERE },

	OperatorDef { "starts_with_cs ",  false,     false,      StringFilter::Position::PREFIX },
	OperatorDef { "!starts_with_cs ", false,     true,       StringFilter::Position::PREFIX },
	OperatorDef { "starts_with_ci ",  true,      false,      StringFilter::Position::PREFIX },
	OperatorDef { "!starts_with_ci ", true,      true,       StringFilter::Position::PREFIX },

	OperatorDef { "eq_cs ",           false,     false,      StringFilter::Position::FULL },
	OperatorDef { "!eq_cs ",          false,     true,       StringFilter::Position::FULL },
	OperatorDef { "eq_ci ",           true,      false,      StringFilter::Position::FULL },
	OperatorDef { "!eq_ci ",          true,      true,       StringFilter::Position::FULL },
};

/**
 * Parse a string operator and its second operand and convert it to a
 * #StringFilter.
 *
 * Throws on error.
 */
static StringFilter
ParseStringFilter(const char *&s, bool fold_case)
{
	for (auto& op: operators) {
		if (auto after_prefix = StringAfterPrefixIgnoreCase(s, op.prefix)) {
			s = StripLeft(after_prefix);
			return StringFilter(
				ExpectQuoted(s),
				op.fold_case,
				op.position,
				op.negated);
		}
	}

	if (auto after_contains = StringAfterPrefixIgnoreCase(s, "contains ")) {
		s = StripLeft(after_contains);
		auto value = ExpectQuoted(s);
		return {
			std::move(value), fold_case,
			StringFilter::Position::ANYWHERE,
			false,
		};
	}

	if (auto after_not_contains = StringAfterPrefixIgnoreCase(s, "!contains ")) {
		s = StripLeft(after_not_contains);
		auto value = ExpectQuoted(s);
		return {
			std::move(value), fold_case,
			StringFilter::Position::ANYWHERE,
			true,
		};
	}

	if (auto after_starts_with = StringAfterPrefixIgnoreCase(s, "starts_with ")) {
		s = StripLeft(after_starts_with);
		auto value = ExpectQuoted(s);
		return {
			std::move(value), fold_case,
			StringFilter::Position::PREFIX,
			false,
		};
	}

	if (auto after_not_starts_with = StringAfterPrefixIgnoreCase(s, "!starts_with ")) {
		s = StripLeft(after_not_starts_with);
		auto value = ExpectQuoted(s);
		return {
			std::move(value), fold_case,
			StringFilter::Position::PREFIX,
			true,
		};
	}

	bool negated = false;

#ifdef HAVE_PCRE
	if ((s[0] == '!' || s[0] == '=') && s[1] == '~') {
		negated = s[0] == '!';
		s = StripLeft(s + 2);
		auto value = ExpectQuoted(s);
		StringFilter f{
			std::move(value), fold_case,
			StringFilter::Position::FULL,
			negated,
		};
		f.SetRegex(std::make_shared<UniqueRegex>(f.GetValue().c_str(),
							 Pcre::CompileOptions{.caseless=fold_case}));
		return f;
	}
#endif

	if (s[0] == '!' && s[1] == '=')
		negated = true;
	else if (s[0] != '=' || s[1] != '=')
		throw FmtRuntimeError("Unknown filter operator: {}", s);

	s = StripLeft(s + 2);
	auto value = ExpectQuoted(s);

	return {
		std::move(value), fold_case,
		StringFilter::Position::FULL,
		negated,
	};
}

ISongFilterPtr
SongFilter::ParseExpression(const char *&s, bool fold_case)
{
	assert(*s == '(');

	s = StripLeft(s + 1);

	if (*s == '(') {
		auto first = ParseExpression(s, fold_case);
		if (*s == ')') {
			s = StripLeft(s + 1);
			return first;
		}

		if (ExpectWord(s) != "AND")
			throw std::runtime_error("'AND' expected");

		auto and_filter = std::make_unique<AndSongFilter>();
		and_filter->AddItem(std::move(first));

		while (true) {
			and_filter->AddItem(ParseExpression(s, fold_case));

			if (*s == ')') {
				s = StripLeft(s + 1);
				return and_filter;
			}

			if (ExpectWord(s) != "AND")
				throw std::runtime_error("'AND' expected");
		}
	}

	if (*s == '!') {
		s = StripLeft(s + 1);

		if (*s != '(')
			throw std::runtime_error("'(' expected");

		auto inner = ParseExpression(s, fold_case);
		if (*s != ')')
			throw std::runtime_error("')' expected");
		s = StripLeft(s + 1);

		return std::make_unique<NotSongFilter>(std::move(inner));
	}

	auto type = ExpectFilterType(s);

	if (type == LOCATE_TAG_MODIFIED_SINCE) {
		const auto value_s = ExpectQuoted(s);
		if (*s != ')')
			throw std::runtime_error("')' expected");
		s = StripLeft(s + 1);
		return std::make_unique<ModifiedSinceSongFilter>(ParseTimeStamp(value_s.c_str()));
	} else if (type == LOCATE_TAG_ADDED_SINCE) {
		const auto value_s = ExpectQuoted(s);
		if (*s != ')')
			throw std::runtime_error("')' expected");
		s = StripLeft(s + 1);
		return std::make_unique<AddedSinceSongFilter>(ParseTimeStamp(value_s.c_str()));
	} else if (type == LOCATE_TAG_BASE_TYPE) {
		auto value = ExpectQuoted(s);
		if (*s != ')')
			throw std::runtime_error("')' expected");
		s = StripLeft(s + 1);

		return std::make_unique<BaseSongFilter>(std::move(value));
	} else if (type == LOCATE_TAG_AUDIO_FORMAT) {
		bool mask;
		if (s[0] == '=' && s[1] == '=')
			mask = false;
		else if (s[0] == '=' && s[1] == '~')
			mask = true;
		else
			throw std::runtime_error("'==' or '=~' expected");

		s = StripLeft(s + 2);

		const auto value = ParseAudioFormat(ExpectQuoted(s).c_str(),
						    mask);

		if (*s != ')')
			throw std::runtime_error("')' expected");
		s = StripLeft(s + 1);

		return std::make_unique<AudioFormatSongFilter>(value);
	} else if (type == LOCATE_TAG_PRIORITY) {
		if (s[0] == '>' && s[1] == '=') {
			// TODO support more operators
		} else
			throw std::runtime_error("'>=' expected");

		s = StripLeft(s + 2);

		char *endptr;
		const auto value = strtoul(s, &endptr, 10);
		if (endptr == s)
			throw std::runtime_error("Number expected");

		if (value > 0xff)
			throw std::runtime_error("Invalid priority value");

		if (*endptr != ')')
			throw std::runtime_error("')' expected");
		s = StripLeft(endptr + 1);

		return std::make_unique<PrioritySongFilter>(value);
	} else {
		auto string_filter = ParseStringFilter(s, fold_case);
		if (*s != ')')
			throw std::runtime_error("')' expected");

		s = StripLeft(s + 1);

		if (type == LOCATE_TAG_ANY_TYPE)
			type = TAG_NUM_OF_ITEM_TYPES;

		if (type == LOCATE_TAG_FILE_TYPE)
			return std::make_unique<UriSongFilter>(std::move(string_filter));

		return std::make_unique<TagSongFilter>(TagType(type),
						       std::move(string_filter));
	}
}

void
SongFilter::Parse(const char *tag_string, const char *value, bool fold_case)
{
	unsigned tag = locate_parse_type(tag_string);

	switch (tag) {
	case TAG_NUM_OF_ITEM_TYPES:
		throw std::runtime_error("Unknown filter type");

	case LOCATE_TAG_BASE_TYPE:
		if (!uri_safe_local(value))
			throw std::runtime_error("Bad URI");

		and_filter.AddItem(std::make_unique<BaseSongFilter>(value));
		break;

	case LOCATE_TAG_MODIFIED_SINCE:
		and_filter.AddItem(std::make_unique<ModifiedSinceSongFilter>(ParseTimeStamp(value)));
		break;
	
	case LOCATE_TAG_ADDED_SINCE:
		and_filter.AddItem(std::make_unique<AddedSinceSongFilter>(ParseTimeStamp(value)));
		break;

	case LOCATE_TAG_FILE_TYPE:
		/* for compatibility with MPD 0.20 and older,
		   "fold_case" also switches on "substring" */
		and_filter.AddItem(std::make_unique<UriSongFilter>(StringFilter{
					value,
					fold_case,
					fold_case
					? StringFilter::Position::ANYWHERE
					: StringFilter::Position::FULL,
					false,
				}));
		break;

	default:
		if (tag == LOCATE_TAG_ANY_TYPE)
			tag = TAG_NUM_OF_ITEM_TYPES;

		/* for compatibility with MPD 0.20 and older,
		   "fold_case" also switches on "substring" */
		and_filter.AddItem(std::make_unique<TagSongFilter>(TagType(tag), StringFilter{
					value,
					fold_case,
					fold_case
					? StringFilter::Position::ANYWHERE
					: StringFilter::Position::FULL,
					false,
				}));
		break;
	}
}

void
SongFilter::Parse(std::span<const char *const> args, bool fold_case)
{
	if (args.empty())
		throw std::runtime_error("Incorrect number of filter arguments");

	do {
		if (*args.front() == '(') {
			const char *s = args.front();
			args = args.subspan(1);
			const char *end = s;
			auto f = ParseExpression(end, fold_case);
			if (*end != 0)
				throw std::runtime_error("Unparsed garbage after expression");

			and_filter.AddItem(std::move(f));
			continue;
		}

		if (args.size() < 2)
			throw std::runtime_error("Incorrect number of filter arguments");

		const char *tag = args[0];
		const char *value = args[1];
		args = args.subspan(2);
		Parse(tag, value, fold_case);
	} while (!args.empty());
}

void
SongFilter::Optimize() noexcept
{
	OptimizeSongFilter(and_filter);
}

bool
SongFilter::Match(const LightSong &song) const noexcept
{
	return and_filter.Match(song);
}

bool
SongFilter::HasFoldCase() const noexcept
{
	return std::any_of(
		and_filter.GetItems().begin(), and_filter.GetItems().end(),
		[](const auto &item) {
			if (auto t = dynamic_cast<const TagSongFilter *>(item.get()))
				return t->GetFoldCase();

			if (auto u = dynamic_cast<const UriSongFilter *>(item.get()))
				return u->GetFoldCase();

			return false;
		});
}

bool
SongFilter::HasOtherThanBase() const noexcept
{
	return std::any_of(and_filter.GetItems().begin(), and_filter.GetItems().end(),
			   [=](const auto &item) {
				   return !dynamic_cast<const BaseSongFilter *>(
					   item.get());
			   });
}

const char *
SongFilter::GetBase() const noexcept
{
	for (const auto &i : and_filter.GetItems()) {
		const auto *f = dynamic_cast<const BaseSongFilter *>(i.get());
		if (f != nullptr)
			return f->GetValue();
	}

	return nullptr;
}

SongFilter
SongFilter::WithoutBasePrefix(const std::string_view prefix) const noexcept
{
	SongFilter result;

	for (const auto &i : and_filter.GetItems()) {
		const auto *f = dynamic_cast<const BaseSongFilter *>(i.get());
		if (f != nullptr) {
			const char *s = StringAfterPrefix(f->GetValue(), prefix);
			if (s != nullptr) {
				if (*s == 0)
					continue;

				if (*s == '/') {
					++s;

					if (*s != 0)
						result.and_filter.AddItem(std::make_unique<BaseSongFilter>(s));

					continue;
				}
			}
		}

		result.and_filter.AddItem(i->Clone());
	}

	return result;
}
