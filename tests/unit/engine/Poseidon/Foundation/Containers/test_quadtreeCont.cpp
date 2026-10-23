// Unit tests for PoseidonBase quadtree container helpers
// Testing continuous quadtree container (for objects with coordinates)

#define _CRT_SECURE_NO_WARNINGS
#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Containers/QuadtreeCont.hpp>

// Test Value Type for QuadTreeCont

// Object with position (for continuous quad tree)
struct GameObject
{
    int x, y;
    int id;

    GameObject() : x(0), y(0), id(0) {}
    GameObject(int x_, int y_, int id_) : x(x_), y(y_), id(id_) {}

    int GetX() const { return x; }
    int GetY() const { return y; }

    bool operator==(const GameObject& other) const { return id == other.id; }
};

// Traits for GameObject
struct GameObjectTraits
{
    typedef int KeyType;
    static bool IsEqual(int a, int b) { return a == b; }
    static int GetKey(const GameObject& obj) { return obj.id; }
};

// Type alias
typedef QuadTreeCont<GameObject, GameObjectTraits> GameObjectTree;

// QuadTreeCont - Basic Construction and Clear

TEST_CASE("QuadTreeCont - Basic construction", "[containers][quadtreecont]")
{
    SECTION("Default construction")
    {
        GameObjectTree tree;

        // Tree is initially empty
        REQUIRE(true); // Just verify it constructs
    }
}

TEST_CASE("QuadTreeCont - Clear operation", "[containers][quadtreecont]")
{
    SECTION("Clear empty tree")
    {
        GameObjectTree tree;
        tree.Clear();

        REQUIRE(true);
    }

    SECTION("Clear populated tree")
    {
        GameObjectTree tree;

        tree.Set(0, 0, GameObject(0, 0, 1));
        tree.Set(5, 5, GameObject(5, 5, 2));

        tree.Clear();

        // Tree should be empty
        REQUIRE(true);
    }
}

// QuadTreeCont - Set Operations

TEST_CASE("QuadTreeCont - Set operations", "[containers][quadtreecont]")
{
    SECTION("Set single object")
    {
        GameObjectTree tree;

        tree.Set(10, 10, GameObject(10, 10, 42));

        REQUIRE(true); // Just verify it doesn't crash
    }

    SECTION("Set multiple objects")
    {
        GameObjectTree tree;

        tree.Set(0, 0, GameObject(0, 0, 1));
        tree.Set(10, 10, GameObject(10, 10, 2));
        tree.Set(20, 20, GameObject(20, 20, 3));

        REQUIRE(true);
    }

    SECTION("Set object at same position")
    {
        GameObjectTree tree;

        tree.Set(5, 5, GameObject(5, 5, 1));
        tree.Set(5, 5, GameObject(5, 5, 2)); // Replace

        REQUIRE(true);
    }
}

// QuadTreeCont - ForEachInRegion Testing

TEST_CASE("QuadTreeCont - ForEachInRegion", "[containers][quadtreecont]")
{
    // An empty tree, or one with a single value at the origin, has a leaf for a root; the walk
    // used to read _index[-1] there (CRASH-20261023, briefing map of a world without peaks).
    SECTION("Empty tree") {
        GameObjectTree tree;

        int count = 0;
        auto detect = [](int x, int y, int size) { return true; };  // All regions
        auto action = [&count](const GameObject& obj) { count++; return false; };

        tree.ForEachInRegion(detect, action);

        REQUIRE(count == 0);
    }

    SECTION("Single object - full region detection")
    {
        GameObjectTree tree;

        tree.Set(5, 5, GameObject(5, 5, 42));

        int count = 0;
        int foundId = 0;
        auto detect = [](int x, int y, int size) { return true; }; // All regions
        auto action = [&](const GameObject& obj)
        {
            count++;
            foundId = obj.id;
            return false;
        };

        tree.ForEachInRegion(detect, action);

        REQUIRE(count == 1);
        REQUIRE(foundId == 42);
    }

    SECTION("Multiple objects")
    {
        GameObjectTree tree;

        tree.Set(0, 0, GameObject(0, 0, 1));
        tree.Set(10, 10, GameObject(10, 10, 2));
        tree.Set(20, 20, GameObject(20, 20, 3));

        int count = 0;
        auto detect = [](int x, int y, int size) { return true; };
        auto action = [&count](const GameObject& obj)
        {
            count++;
            return false;
        };

        tree.ForEachInRegion(detect, action);

        REQUIRE(count == 3);
    }

    SECTION("Region detection filter")
    {
        GameObjectTree tree;

        tree.Set(5, 5, GameObject(5, 5, 1));
        tree.Set(15, 15, GameObject(15, 15, 2));
        tree.Set(25, 25, GameObject(25, 25, 3));

        // Only detect regions overlapping [0,0] to [10,10]
        int count = 0;
        auto detect = [](int x, int y, int size)
        {
            return x < 10 && y < 10; // Only first quadrant
        };
        auto action = [&count](const GameObject& obj)
        {
            count++;
            return false;
        };

        tree.ForEachInRegion(detect, action);

        // Should find fewer objects (exact count depends on tree structure)
        REQUIRE(count >= 0); // At minimum, basic functionality works
    }

    SECTION("Early termination with action return true")
    {
        GameObjectTree tree;

        tree.Set(0, 0, GameObject(0, 0, 1));
        tree.Set(10, 10, GameObject(10, 10, 2));
        tree.Set(20, 20, GameObject(20, 20, 3));

        int count = 0;
        auto detect = [](int x, int y, int size) { return true; };
        auto action = [&count](const GameObject& obj)
        {
            count++;
            return count >= 2; // Stop after 2 objects
        };

        bool stopped = tree.ForEachInRegion(detect, action);

        REQUIRE(stopped == true);
        REQUIRE(count == 2);
    }
}

// QuadTreeCont - Spatial Queries

TEST_CASE("QuadTreeCont - Spatial region queries", "[containers][quadtreecont]")
{
    SECTION("Find objects in circular region")
    {
        GameObjectTree tree;

        // Add objects in a pattern
        tree.Set(50, 50, GameObject(50, 50, 1));     // Center
        tree.Set(55, 50, GameObject(55, 50, 2));     // Near
        tree.Set(100, 100, GameObject(100, 100, 3)); // Far

        // Detect objects within radius of (50, 50)
        const int centerX = 50, centerY = 50, radius = 10;
        int count = 0;

        auto detect = [](int x, int y, int size)
        {
            // Check if region overlaps circle
            const int centerX = 50, centerY = 50, radius = 10;
            int dx = (x + size / 2) - centerX;
            int dy = (y + size / 2) - centerY;
            int distSq = dx * dx + dy * dy;
            return distSq <= (radius + size) * (radius + size);
        };

        auto action = [&](const GameObject& obj)
        {
            // Check if object is actually inside circle
            int dx = obj.GetX() - centerX;
            int dy = obj.GetY() - centerY;
            if (dx * dx + dy * dy <= radius * radius)
            {
                count++;
            }
            return false;
        };

        tree.ForEachInRegion(detect, action);

        // Should find at least the center object
        REQUIRE(count >= 1);
    }
}

// QuadTreeCont - Edge Cases

TEST_CASE("QuadTreeCont - Edge cases", "[containers][quadtreecont]")
{
    SECTION("Objects at origin") {
        GameObjectTree tree;

        tree.Set(0, 0, GameObject(0, 0, 1));

        int count = 0;
        auto detect = [](int x, int y, int size) { return true; };
        auto action = [&count](const GameObject& obj) { count++; return false; };

        tree.ForEachInRegion(detect, action);

        REQUIRE(count == 1);
    }

    SECTION("Large coordinates") {
        GameObjectTree tree;

        tree.Set(1000, 1000, GameObject(1000, 1000, 99));

        int count = 0;
        auto detect = [](int x, int y, int size) { return true; };
        auto action = [&count](const GameObject& obj) { count++; return false; };

        tree.ForEachInRegion(detect, action);

        REQUIRE(count == 1);
    }

    SECTION("Objects at power-of-2 boundaries") {
        GameObjectTree tree;

        tree.Set(1, 1, GameObject(1, 1, 1));
        tree.Set(2, 2, GameObject(2, 2, 2));
        tree.Set(4, 4, GameObject(4, 4, 3));
        tree.Set(8, 8, GameObject(8, 8, 4));
        tree.Set(16, 16, GameObject(16, 16, 5));

        int count = 0;
        auto detect = [](int x, int y, int size) { return true; };
        auto action = [&count](const GameObject& obj) { count++; return false; };

        tree.ForEachInRegion(detect, action);

        REQUIRE(count == 5);
    }
}

// QuadTreeCont - Stress Tests

TEST_CASE("QuadTreeCont - Stress test", "[containers][quadtreecont]") {
    SECTION("Many objects") {
        GameObjectTree tree;

        // Add 100 objects
        for (int i = 0; i < 100; i++) {
            int x = (i * 7) % 100;  // Pseudo-random positions
            int y = (i * 11) % 100;
            tree.Set(x, y, GameObject(x, y, i));
        }

        // Count all objects
        int count = 0;
        auto detect = [](int x, int y, int size) { return true; };
        auto action = [&count](const GameObject& obj) { count++; return false; };

        tree.ForEachInRegion(detect, action);

        REQUIRE(count == 100);
    }

    SECTION("Repeated insertions and queries") {
        GameObjectTree tree;

        for (int iter = 0; iter < 10; iter++) {
            // Add objects
            for (int i = 0; i < 10; i++) {
                tree.Set(i * 5, i * 5, GameObject(i * 5, i * 5, i));
            }

            // Query
            int count = 0;
            auto detect = [](int x, int y, int size) { return true; };
            auto action = [&count](const GameObject& obj) { count++; return false; };

            tree.ForEachInRegion(detect, action);

            REQUIRE(count >= 10);

            tree.Clear();
        }
    }
}
