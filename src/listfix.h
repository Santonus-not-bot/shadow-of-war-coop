// The game's intrusive reference lists (session cm). An object of the base class with vtable 1DB7358 (a character, among
// others) keeps a list of the references that point at it: a circular doubly linked list whose head is at object+8 (next,
// prev). A reference is { vtable; node: next, prev; the node's owner (the reference itself); target (+0x20) }, its node at
// reference+8. The game's 18D264 unlinks a reference only when its target is set, and the destructor 4DD18C empties the list by
// calling 18D264 on the last reference until the list is empty: a reference that is linked with its target 0 is never unlinked
// and the loop never ends (the client's hang, sessions ck-cm).
//
// ReferenceUnlink does what 18D264 does when the target is set; when it is 0 and the reference's node is linked into a list
// whose neighbours point back at it, it unlinks the node and links it to itself (the state 18D264 leaves). Pure pointer work:
// the caller guards the reads (the game's memory) with SEH.
#pragma once
#include <cstdint>

namespace listfix {

enum class Unlink { kNormal, kRepaired, kLeft };

// `ref` is the reference's address. Returns what was done: kNormal (target set: unlinked, target cleared), kRepaired (target 0,
// node linked: unlinked), kLeft (target 0 and the node unlinked or its neighbours not pointing back: nothing written).
inline Unlink ReferenceUnlink(uintptr_t ref) {
  auto at = [](uintptr_t a) -> uintptr_t& { return *reinterpret_cast<uintptr_t*>(a); };
  const uintptr_t node = ref + 8;
  const uintptr_t next = at(node), prev = at(node + 8);
  const bool target = at(ref + 0x20) != 0;
  if (!target) {
    if (next == node || !next || !prev || at(next + 8) != node || at(prev) != node) return Unlink::kLeft;
  }
  at(next + 8) = prev;
  at(prev) = next;
  at(node + 8) = node;
  at(node) = node;
  if (target) at(ref + 0x20) = 0;
  return target ? Unlink::kNormal : Unlink::kRepaired;
}

}  // namespace listfix
