#pragma once

#include <Kernel/Types.h>

// Doubly linked list structure
typedef struct ListHead {
    struct ListHead *Next;
    struct ListHead *Prev;
} ListHead;

static inline NOPTR ListInit(ListHead *Head) {
    Head->Next = Head;
    Head->Prev = Head;
}

static inline NOPTR ListAdd(ListHead *Head, ListHead *Node) {
    Node->Next = Head->Next;
    Node->Prev = Head;
    Head->Next->Prev = Node;
    Head->Next = Node;
}

static inline NOPTR ListAddTail(ListHead *Head, ListHead *Node) {
    Node->Next = Head;
    Node->Prev = Head->Prev;
    Head->Prev->Next = Node;
    Head->Prev = Node;
}

static inline NOPTR ListDel(ListHead *Node) {
    Node->Prev->Next = Node->Next;
    Node->Next->Prev = Node->Prev;
    Node->Next = NULLPTR;
    Node->Prev = NULLPTR;
}

static inline BOOL ListEmpty(ListHead *Head) {
    return Head->Next == Head;
}

#define ListEntry(Ptr, Type, Member) \
    ((Type *)((CHAR *)(Ptr) - OffsetOf(Type, Member)))

#define ListForEach(Pos, Head) \
    for (Pos = (Head)->Next; Pos != (Head); Pos = Pos->Next)

#define ListForEachSafe(Pos, N, Head) \
    for ((Pos) = (Head)->Next, (N) = (Pos)->Next; (Pos) != (Head); (Pos) = (N), (N) = (Pos)->Next)
