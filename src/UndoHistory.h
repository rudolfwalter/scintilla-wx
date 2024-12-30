// Scintilla source code edit control
/** @file UndoHistory.h
 ** Manages undo for the document.
 **/
// Copyright 1998-2024 by Neil Hodgson <neilh@scintilla.org>
// The License.txt file describes the conditions under which this software may be distributed.

#ifndef UNDOHISTORY_H
#define UNDOHISTORY_H

namespace Scintilla { namespace Internal {

// ScaledVector is a vector of unsigned integers that uses elements sized to hold the largest value.
// Thus, if an undo history only contains short insertions and deletions the lengths vector may
// only use 2 bytes or even 1 byte for each length.
// This saves much memory often reducing by 50% for 32-bit builds and 75% for 64-bit builds.

struct SizeMax {
	size_t size = 1;
	size_t maxValue = UINT8_MAX;

	constexpr SizeMax(size_t size = 1, size_t maxValue = UINT8_MAX) : size(size), maxValue(maxValue) { }
};

class ScaledVector {
	SizeMax element;
	std::vector<uint8_t> bytes;
public:
	SCI_NODISCARD size_t Size() const noexcept;
	SCI_NODISCARD size_t ValueAt(size_t index) const noexcept;
	SCI_NODISCARD intptr_t SignedValueAt(size_t index) const noexcept;
	void SetValueAt(size_t index, size_t value);
	void ClearValueAt(size_t index) noexcept;
	void Clear() noexcept;
	void Truncate(size_t length) noexcept;
	void ReSize(size_t length);
	void PushBack();

	// For testing
	SCI_NODISCARD size_t SizeInBytes() const noexcept;
};

class UndoActionType {
public:
	ActionType at : 4;
	bool mayCoalesce : 1;
	UndoActionType() noexcept;
};

struct UndoActions {
	std::vector<UndoActionType> types;
	ScaledVector positions;
	ScaledVector lengths;

	UndoActions();
	void Truncate(size_t length) noexcept;
	void PushBack();
	void Clear() noexcept;
	SCI_NODISCARD intptr_t SSize() const noexcept;
	void Create(size_t index, ActionType at_, Sci::Position position_, Sci::Position lenData_, bool mayCoalesce_);
	SCI_NODISCARD bool AtStart(size_t index) const noexcept;
	SCI_NODISCARD size_t LengthTo(size_t index) const noexcept;
	SCI_NODISCARD Sci::Position Position(int action) const noexcept;
	SCI_NODISCARD Sci::Position Length(int action) const noexcept;
};

class ScrapStack {
	std::string stack;
	size_t current = 0;
public:
	void Clear() noexcept;
	const char *Push(const char *text, size_t length);
	void SetCurrent(size_t position) noexcept;
	void MoveForward(size_t length) noexcept;
	void MoveBack(size_t length) noexcept;
	SCI_NODISCARD const char *CurrentText() const noexcept;
	SCI_NODISCARD const char *TextAt(size_t position) const noexcept;
};

constexpr int coalesceFlag = 0x100;

/**
 *
 */
class UndoHistory {
	UndoActions actions;
	int currentAction = 0;
	int undoSequenceDepth = 0;
	int savePoint = 0;
	int tentativePoint = -1;
	Sci::optional<int> detach;	// Never set if savePoint set (>= 0)
	std::unique_ptr<ScrapStack> scraps;
	struct actPos { int act; size_t position; };
	Sci::optional<actPos> memory;

	int PreviousAction() const noexcept;

public:
	UndoHistory();
	~UndoHistory() noexcept;

	const char *AppendAction(ActionType at, Sci::Position position, const char *data, Sci::Position lengthData, bool &startSequence, bool mayCoalesce=true);

	void BeginUndoAction(bool mayCoalesce=false) noexcept;
	void EndUndoAction() noexcept;
	int UndoSequenceDepth() const noexcept;
	void DropUndoSequence() noexcept;
	void DeleteUndoHistory() noexcept;

	SCI_NODISCARD int Actions() const noexcept;

	/// The save point is a marker in the undo stack where the container has stated that
	/// the buffer was saved. Undo and redo can move over the save point.
	void SetSavePoint(int action) noexcept;
	SCI_NODISCARD int SavePoint() const noexcept;
	void SetSavePoint() noexcept;
	bool IsSavePoint() const noexcept;
	bool BeforeSavePoint() const noexcept;
	bool PreviousBeforeSavePoint() const noexcept;
	bool BeforeReachableSavePoint() const noexcept;
	bool AfterSavePoint() const noexcept;

	/// The detach point is the last action that was before an inaccessible missing save point.
	void SetDetachPoint(int action) noexcept;
	SCI_NODISCARD int DetachPoint() const noexcept;
	bool AfterDetachPoint() const noexcept;
	bool AfterOrAtDetachPoint() const noexcept;

	SCI_NODISCARD intptr_t Delta(int action) const noexcept;
	SCI_NODISCARD bool Validate(intptr_t lengthDocument) const noexcept;
	void SetCurrent(int action, intptr_t lengthDocument);
	SCI_NODISCARD int Current() const noexcept;
	SCI_NODISCARD int Type(int action) const noexcept;
	SCI_NODISCARD Sci::Position Position(int action) const noexcept;
	SCI_NODISCARD Sci::Position Length(int action) const noexcept;
	SCI_NODISCARD Sci::string_view Text(int action) noexcept;
	void PushUndoActionType(int type, Sci::Position position);
	void ChangeLastUndoActionText(size_t length, const char *text);

	// Tentative actions are used for input composition so that it can be undone cleanly
	void SetTentative(int action) noexcept;
	SCI_NODISCARD int TentativePoint() const noexcept;
	void TentativeStart() noexcept;
	void TentativeCommit() noexcept;
	bool TentativeActive() const noexcept;
	int TentativeSteps() const noexcept;

	/// To perform an undo, StartUndo is called to retrieve the number of steps, then UndoStep is
	/// called that many times. Similarly for redo.
	bool CanUndo() const noexcept;
	int StartUndo() const noexcept;
	Action GetUndoStep() const noexcept;
	void CompletedUndoStep() noexcept;
	bool CanRedo() const noexcept;
	int StartRedo() const noexcept;
	Action GetRedoStep() const noexcept;
	void CompletedRedoStep() noexcept;
};

}}

#endif
