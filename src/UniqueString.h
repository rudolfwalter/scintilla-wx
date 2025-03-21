// Scintilla source code edit control
/** @file UniqueString.h
 ** Define UniqueString, a unique_ptr based string type for storage in containers
 ** and an allocator for UniqueString.
 ** Define UniqueStringSet which holds a set of strings, used to avoid holding many copies
 ** of font names.
 **/
// Copyright 2017 by Neil Hodgson <neilh@scintilla.org>
// The License.txt file describes the conditions under which this software may be distributed.

#ifndef UNIQUESTRING_H
#define UNIQUESTRING_H

namespace Scintilla { namespace Internal {

constexpr bool IsNullOrEmpty(const char *text) noexcept {
	return text == nullptr || *text == '\0';
}

using UniqueString = std::unique_ptr<const char[]>;

/// Equivalent to strdup but produces a std::unique_ptr<const char[]> allocation to go
/// into collections.
UniqueString UniqueStringCopy(const char *text);

// A set of strings that always returns the same pointer for each string.

class UniqueStringSet {
private:
	std::vector<UniqueString> strings;
public:
	UniqueStringSet();
	void Clear() noexcept;
	const char *Save(const char *text);
};

}}

#endif
