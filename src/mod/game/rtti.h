#pragma once
// MSVC RTTI lookups inside the game process (read-only, guarded reads).

#include <cstdint>
#include <string>

namespace cdmp::game {

// Decorated class name (".?AVName@pa@@") of the object at `obj`, or "".
std::string rtti_name_of_object(uintptr_t obj);

// Same, starting from a vtable address.
std::string rtti_name_of_vtable(uintptr_t vtable);

// ".?AVTrocTrMoveActorReq@pa@@" -> "MoveActorReq" (strips namespace and the
// given prefix); returns the input unchanged if it doesn't match.
std::string short_class_name(const std::string& decorated, const char* strip_prefix = "");

}  // namespace cdmp::game
