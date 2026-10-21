// RFG-128: a parent prefab's inline MaterialAssignClass on a socket child belongs to that slot.
#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Asset/Formats/Enfusion/PrefabResolve.hpp>

#include <string>
#include <vector>

using namespace Poseidon::Asset::Formats::Enfusion;

namespace
{
std::vector<uint8_t> Bytes(const std::string& text)
{
    return {text.begin(), text.end()};
}

// The shape of Door_Village_E_B_LEFT_INT_ST_B.et: the glass hangs on a pivot and the door
// re-materials it in the child's own block. The root's own MeshObject is not a slot.
const char* kDoor = R"(GenericEntity : "{F1793FE006FDF888}Prefabs/Door_Base.et" {
 ID "527C87049FF9E62F"
 components {
  MeshObject "{309366462BDAFB24}" {
   Object "{30836FF2F6B8F83F}Assets/Door.xob"
  }
 }
 {
  GenericEntity : "{607594FC45F03E08}Prefabs/Glass_102x58.et" {
   ID "572C9CEF021A9412"
   components {
    MeshObject "{51C4DA208DB0E200}" {
     Materials {
      MaterialAssignClass "{5856B7490010A1E2}" {
       SourceMaterial "Assets/Windows/Data/window_glass"
       AssignedMaterial "{DFF3A8696381DD38}Assets/Windows/Data/window_glass_uneven_INT.emat"
      }
     }
    }
    Hierarchy "{51C4DA20F011E284}" {
     PivotID "Socket_Glass_102x58"
    }
   }
  }
  GenericEntity : "{AAAA}Prefabs/Handle.et" {
   ID "572C9CEF021A9413"
   components {
    Hierarchy "{51C4DA20F011E285}" {
     PivotID "Socket_Handle"
    }
   }
  }
 }
}
)";
} // namespace

TEST_CASE("Prefab slots carry the parent's inline material override of the part", "[enfusion][prefab][rfg-128]")
{
    std::vector<PrefabSlot> exact, prefixes;
    FindSlotDeclarations(Bytes(kDoor), exact, prefixes);
    REQUIRE(exact.size() == 2);
    REQUIRE(exact[0].bone == "socket_glass_102x58");
    REQUIRE(exact[0].prefab == "Prefabs/Glass_102x58.et"); // GUID stripped
    REQUIRE(exact[0].materialRemap.size() == 1);
    CHECK(exact[0].materialRemap[0].first == "Assets/Windows/Data/window_glass");
    CHECK(exact[0].materialRemap[0].second == "Assets/Windows/Data/window_glass_uneven_INT.emat");
    // The sibling declares nothing and must not inherit the glass's override.
    REQUIRE(exact[1].bone == "socket_handle");
    CHECK(exact[1].materialRemap.empty());

    PrefabSlots slots;
    slots.exact = exact;
    const PrefabSlot* glass = SlotForBone(slots, "Socket_Glass_102x58");
    REQUIRE(glass != nullptr);
    CHECK(glass->materialRemap.size() == 1);
    CHECK(SlotForBone(slots, "socket_nothing") == nullptr);
    CHECK(SlotPrefabForBone(slots, "socket_handle") == "Prefabs/Handle.et");
}

TEST_CASE("Prefab entity Flags are the root entity's own (set, clear) pair", "[enfusion][prefab][rfg-130]")
{
    // Building_Base.et: the entity sets RELATIVE_Y (0x80000); a component's own Flags is not it.
    const char* building = R"(GenericEntity {
 ID "1"
 components {
  RigidBody "{2}" {
   Flags 0 0x1
  }
 }
 Flags 0x80403 0
}
)";
    uint32_t set = 0, clear = 0;
    REQUIRE(FindEntityFlags(Bytes(building), set, clear));
    CHECK(set == 0x80403u);
    CHECK(clear == 0u);

    const char* props = "GenericEntity {\n Flags 0x403 0\n}\n";
    REQUIRE(FindEntityFlags(Bytes(props), set, clear));
    CHECK((set & 0x80000u) == 0u);

    const char* none = "GenericEntity : \"{A}Prefabs/X.et\" {\n ID \"3\"\n}\n";
    CHECK_FALSE(FindEntityFlags(Bytes(none), set, clear));
}
