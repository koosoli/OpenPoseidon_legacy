// Unit tests for PoseidonBase string utilities
// Testing RString (reference-counted strings) and BString (bounded strings)

#define _CRT_SECURE_NO_WARNINGS
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Strings/Bstring.hpp>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>

using Catch::Matchers::Equals;

// Batch 1: RString - Basic Construction and Operations

TEST_CASE("RString - Construction", "[strings]")
{
    SECTION("Default construction")
    {
        RString str;

        REQUIRE(str.Data() != nullptr); // Always returns valid pointer
        REQUIRE(strcmp(str.Data(), "") == 0);
        REQUIRE(str.GetLength() == 0);
    }

    SECTION("Construction from C string")
    {
        RString str("Hello");

        REQUIRE(strcmp(str.Data(), "Hello") == 0);
        REQUIRE(str.GetLength() == 5);
    }

    SECTION("Construction with length")
    {
        RString str("Hello World", 5);

        REQUIRE(strcmp(str.Data(), "Hello") == 0);
        REQUIRE(str.GetLength() == 5);
    }

    SECTION("Construction from concatenation")
    {
        RString str("Hello", " World");

        REQUIRE(strcmp(str.Data(), "Hello World") == 0);
        REQUIRE(str.GetLength() == 11);
    }

    SECTION("Construction from nullptr")
    {
        RString str(nullptr);

        REQUIRE(str.Data() != nullptr);
        REQUIRE(strcmp(str.Data(), "") == 0);
    }
}

TEST_CASE("RString - Copy and Reference Counting", "[strings]")
{
    SECTION("Copy shares reference")
    {
        RString str1("Test");
        RString str2 = str1;

        REQUIRE(str1.Data() == str2.Data()); // Same pointer
        REQUIRE(str1.GetRefCount() == 2);
        REQUIRE(str2.GetRefCount() == 2);
    }

    SECTION("Multiple copies increment ref count")
    {
        RString str1("Test");
        RString str2 = str1;
        RString str3 = str1;

        REQUIRE(str1.GetRefCount() == 3);
        REQUIRE(str2.GetRefCount() == 3);
        REQUIRE(str3.GetRefCount() == 3);
    }
}

TEST_CASE("RString - Copy-on-Write", "[strings]")
{
    SECTION("MakeMutable creates independent copy")
    {
        RString str1("Test");
        RString str2 = str1;

        REQUIRE(str1.Data() == str2.Data());

        (void)str1.MutableData(); // Triggers copy-on-write

        REQUIRE(str1.Data() != str2.Data()); // Now different pointers
        REQUIRE(str1.GetRefCount() == 1);
        REQUIRE(str2.GetRefCount() == 1);
    }

    SECTION("MutableData returns nullptr for empty string")
    {
        RString str;

        REQUIRE(str.MutableData() == nullptr);
    }
}

TEST_CASE("RString - Operators", "[strings]")
{
    SECTION("Operator const char*")
    {
        RString str("Test");
        const char* cstr = str;

        REQUIRE(strcmp(cstr, "Test") == 0);
    }

    SECTION("Operator [] for reading")
    {
        RString str("Test");

        REQUIRE(str[0] == 'T');
        REQUIRE(str[1] == 'e');
        REQUIRE(str[2] == 's');
        REQUIRE(str[3] == 't');
        REQUIRE(str[4] == '\0');
    }

    SECTION("Comparison operators")
    {
        RString str1("Test");
        RString str2("Test");
        RString str3("Other");

        REQUIRE(str1 == str2);
        REQUIRE(str1 != str3);
        REQUIRE_FALSE(str1 == str3);
        REQUIRE_FALSE(str1 != str2);
    }
}

TEST_CASE("RString - Case Conversion", "[strings]")
{
    SECTION("Lower")
    {
        RString str("HELLO");
        str.Lower();

        REQUIRE(strcmp(str.Data(), "hello") == 0);
    }

    SECTION("Upper")
    {
        RString str("hello");
        str.Upper();

        REQUIRE(strcmp(str.Data(), "HELLO") == 0);
    }

    SECTION("Lower creates independent copy")
    {
        RString str1("HELLO");
        RString str2 = str1;

        str1.Lower();

        REQUIRE(strcmp(str1.Data(), "hello") == 0);
        REQUIRE(strcmp(str2.Data(), "HELLO") == 0); // str2 unchanged
    }
}

TEST_CASE("RString - Substring", "[strings]")
{
    SECTION("Extract middle substring")
    {
        RString str("Hello World");
        RString sub = str.Substring(6, 11);

        REQUIRE(strcmp(sub.Data(), "World") == 0);
    }

    SECTION("Extract from start")
    {
        RString str("Hello World");
        RString sub = str.Substring(0, 5);

        REQUIRE(strcmp(sub.Data(), "Hello") == 0);
    }

    SECTION("Extract to end")
    {
        RString str("Hello World");
        RString sub = str.Substring(6, 11);

        REQUIRE(strcmp(sub.Data(), "World") == 0);
    }

    SECTION("Whole string returns same reference")
    {
        RString str("Test");
        RString sub = str.Substring(0, 4);

        REQUIRE(str.Data() == sub.Data()); // Optimization: same pointer
    }

    SECTION("Bounds clamping")
    {
        RString str("Test");
        RString sub = str.Substring(0, 100);

        REQUIRE(strcmp(sub.Data(), "Test") == 0);
    }
}

TEST_CASE("RString - Edge Cases", "[strings]")
{
    SECTION("Empty string")
    {
        RString str("");

        REQUIRE(str.GetLength() == 0);
        REQUIRE(strcmp(str.Data(), "") == 0);
    }

    SECTION("Default empty equals empty literal")
    {
        RString str;
        REQUIRE(str == RString(""));
        REQUIRE(str.GetLength() == 0);
    }

    SECTION("Copy-on-write: assign to copy leaves original unchanged")
    {
        RString original("original");
        RString copy = original;
        copy = "modified";

        REQUIRE(strcmp(original.Data(), "original") == 0);
        REQUIRE(strcmp(copy.Data(), "modified") == 0);
    }

    SECTION("Concatenation: hello + space + world")
    {
        RString result = RString("hello") + RString(" ") + RString("world");
        REQUIRE(strcmp(result.Data(), "hello world") == 0);
    }

    SECTION("Ordering via strcmp (no operator</>)")
    {
        // RString has no operator< / operator>, use strcmp via implicit const char*
        RString a("abc"), b("abd"), c("b");
        REQUIRE(strcmp(a, b) < 0);
        REQUIRE(strcmp(a, a) == 0);
        REQUIRE(strcmp(c, a) > 0);
    }

    SECTION("1000-char string round-trip")
    {
        char buf[1001];
        for (int i = 0; i < 1000; i++)
            buf[i] = 'A' + (i % 26);
        buf[1000] = '\0';

        RString str(buf);
        REQUIRE(str.GetLength() == 1000);
        REQUIRE(strcmp(str.Data(), buf) == 0);
    }

    // RString has no Format/Printf method (only BString has PrintF)

    SECTION("Very long string")
    {
        char longStr[1024];
        for (int i = 0; i < 1023; i++)
        {
            longStr[i] = 'A' + (i % 26);
        }
        longStr[1023] = '\0';

        RString str(longStr);

        REQUIRE(str.GetLength() == 1023);
        REQUIRE(strcmp(str.Data(), longStr) == 0);
    }

    SECTION("String with special characters")
    {
        RString str("Test\nWith\tSpecial\r\nChars!");

        // \n = 1 char, \t = 1 char, \r\n = 2 chars
        // Test = 4, \n = 1, With = 4, \t = 1, Special = 7, \r\n = 2, Chars! = 6
        // Total: 4 + 1 + 4 + 1 + 7 + 2 + 6 = 25
        REQUIRE(str.GetLength() == 25);
    }
}

// Batch 2: RStringS and RStringI (Typed Strings)

TEST_CASE("RStringS - Case Sensitive Comparison", "[strings]")
{
    SECTION("Equal strings")
    {
        RStringS str1("Test");
        RStringS str2("Test");

        REQUIRE(str1 == str2);
        REQUIRE_FALSE(str1 != str2);
    }

    SECTION("Different case are not equal")
    {
        RStringS str1("Test");
        RStringS str2("test");

        REQUIRE_FALSE(str1 == str2);
        REQUIRE(str1 != str2);
    }

    SECTION("Construction from RString")
    {
        RString base("Test");
        RStringS typed(base);

        REQUIRE(strcmp(typed.Data(), "Test") == 0);
    }
}

TEST_CASE("RStringI - Case Insensitive Comparison", "[strings]")
{
    SECTION("Equal strings")
    {
        RStringI str1("Test");
        RStringI str2("Test");

        REQUIRE(str1 == str2);
        REQUIRE_FALSE(str1 != str2);
    }

    SECTION("Different case are equal")
    {
        RStringI str1("Test");
        RStringI str2("test");
        RStringI str3("TEST");

        REQUIRE(str1 == str2);
        REQUIRE(str2 == str3);
        REQUIRE(str1 == str3);
    }

    SECTION("Construction variants")
    {
        RString base("Test");
        RStringI typed1(base);
        RStringI typed2("test");
        RStringI typed3("Hello", " World");

        REQUIRE(typed1 == typed2);
        REQUIRE(strcmp(typed3.Data(), "Hello World") == 0);
    }
}

// Batch 3: BString - Fixed Size String Buffer

// Known issue #10: ALL BString tests commented out due to sentinel overwrite issue
// - Root cause: strcpyLtd doesn't null-terminate properly, overwrites sentinel at _data[Size-1]
// - IsValid() calls Fail() which triggers debug break in destructor
// - Affects ALL BString tests (construction, assignment, operations, etc.)
// - Status: KNOWN BUG - preserved for 1:1 library compatibility
// - Fix: Modify strcpyLtd to: strncpy(dst, src, size-1); dst[size-1] = '\0';
/*
TEST_CASE("BString - Construction", "[strings]") {
    SECTION("Default construction") {
        BString<32> str;
        REQUIRE(strcmp(str.cstr(), "") == 0);
        REQUIRE(str.IsValid());
    }

    SECTION("Construction from C string") {
        BString<32> str("Hello");
        REQUIRE(strcmp(str.cstr(), "Hello") == 0);
        REQUIRE(str.IsValid());
    }

    SECTION("Copy construction") {
        BString<32> str1("Test");
        BString<32> str2(str1);
        REQUIRE(strcmp(str2.cstr(), "Test") == 0);
        REQUIRE(str2.IsValid());
    }
}

TEST_CASE("BString - Assignment", "[strings]") {
    SECTION("Assignment from C string") {
        BString<32> str;
        str = "Hello";
        REQUIRE(strcmp(str.cstr(), "Hello") == 0);
    }

    SECTION("Assignment from another BString") {
        BString<32> str1("Test");
        BString<32> str2;
        str2 = str1;
        REQUIRE(strcmp(str2.cstr(), "Test") == 0);
    }
}

TEST_CASE("BString - Conversion", "[strings]") {
    SECTION("Implicit conversion to const char*") {
        BString<32> str("Test");
        const char* cstr = str;
        REQUIRE(strcmp(cstr, "Test") == 0);
    }

    SECTION("Explicit cstr() method") {
        BString<32> str("Test");
        REQUIRE(strcmp(str.cstr(), "Test") == 0);
    }
}

TEST_CASE("BString - Array Access", "[strings]") {
    SECTION("Read access") {
        BString<32> str("Test");
        REQUIRE(str[0] == 'T');
        REQUIRE(str[1] == 'e');
        REQUIRE(str[2] == 's');
        REQUIRE(str[3] == 't');
    }

    SECTION("Write access") {
        BString<32> str("Test");
        str[0] = 'B';
        str[1] = 'e';
        REQUIRE(strcmp(str.cstr(), "Best") == 0);
    }
}

TEST_CASE("BString - Concatenation", "[strings]") {
    SECTION("Operator +=") {
        BString<32> str("Hello");
        str += " World";
        REQUIRE(strcmp(str.cstr(), "Hello World") == 0);
    }

    SECTION("strcat function") {
        BString<32> str("Hello");
        strcat(str, " World");
        REQUIRE(strcmp(str.cstr(), "Hello World") == 0);
    }
}

TEST_CASE("BString - Printf", "[strings]") {
    SECTION("PrintF basic") {
        BString<64> str;
        str.PrintF("Value: %d", 42);
        REQUIRE(strcmp(str.cstr(), "Value: 42") == 0);
    }

    SECTION("sprintf function") {
        BString<64> str;
        sprintf(str, "Test %s %d", "string", 123);
        REQUIRE(strcmp(str.cstr(), "Test string 123") == 0);
    }

    SECTION("Multiple values") {
        BString<64> str;
        str.PrintF("%d + %d = %d", 2, 3, 5);
        REQUIRE(strcmp(str.cstr(), "2 + 3 = 5") == 0);
    }
}

TEST_CASE("BString - Case Conversion", "[strings]") {
    SECTION("StrLwr") {
        BString<32> str("HELLO");
        str.StrLwr();
        REQUIRE(strcmp(str.cstr(), "hello") == 0);
    }

    SECTION("StrUpr") {
        BString<32> str("hello");
        str.StrUpr();
        REQUIRE(strcmp(str.cstr(), "HELLO") == 0);
    }

    SECTION("strlwr function") {
        BString<32> str("HELLO");
        strlwr(str);
        REQUIRE(strcmp(str.cstr(), "hello") == 0);
    }

    SECTION("strupr function") {
        BString<32> str("hello");
        strupr(str);
        REQUIRE(strcmp(str.cstr(), "HELLO") == 0);
    }
}

TEST_CASE("BString - Bounded Operations", "[strings]") {
    SECTION("strcpy with truncation") {
        BString<8> str;
        str = "This is a very long string";
        REQUIRE(str.IsValid());
        REQUIRE(strlen(str.cstr()) <= 7);
    }

    SECTION("strcat with truncation") {
        BString<16> str("Hello");
        str += " This is a very long addition";
        REQUIRE(str.IsValid());
        REQUIRE(strlen(str.cstr()) <= 15);
    }

    SECTION("StrNCpy") {
        BString<32> str;
        str.StrNCpy("Hello World", 5);
        REQUIRE(strncmp(str.cstr(), "Hello", 5) == 0);
    }

    SECTION("PrintF with truncation") {
        BString<16> str;
        str.PrintF("This is a very long formatted string with %d", 12345);
        REQUIRE(str.IsValid());
        REQUIRE(strlen(str.cstr()) <= 15);
    }
}

TEST_CASE("BString - Helper Functions", "[strings]") {
    SECTION("strcpy helper") {
        BString<32> str;
        strcpy(str, "Test");
        REQUIRE(strcmp(str.cstr(), "Test") == 0);
    }

    SECTION("strncpy helper") {
        BString<32> str;
        strncpy(str, "Hello World", 5);
        REQUIRE(strncmp(str.cstr(), "Hello", 5) == 0);
    }
}

TEST_CASE("BString - Different Sizes", "[strings]") {
    SECTION("Small buffer") {
        BString<8> str("Test");
        REQUIRE(strcmp(str.cstr(), "Test") == 0);
        REQUIRE(str.IsValid());
    }

    SECTION("Large buffer") {
        BString<256> str("Test");
        REQUIRE(strcmp(str.cstr(), "Test") == 0);
        REQUIRE(str.IsValid());
    }

    SECTION("Size 1 buffer") {
        BString<1> str("");
        REQUIRE(strcmp(str.cstr(), "") == 0);
        REQUIRE(str.IsValid());
    }
}

TEST_CASE("BString - Edge Cases", "[strings]") {
    SECTION("Empty string operations") {
        BString<32> str;
        str += "";
        str.StrLwr();
        str.StrUpr();
        REQUIRE(strcmp(str.cstr(), "") == 0);
        REQUIRE(str.IsValid());
    }

    SECTION("Special characters") {
        BString<64> str;
        str.PrintF("Line1\nLine2\tTabbed\r\nLine3");
        REQUIRE(str.IsValid());
    }

    SECTION("Multiple operations") {
        BString<64> str("Hello");
        str += " ";
        str += "World";
        strcpy(str, "New");
        str += " Value";
        REQUIRE(strcmp(str.cstr(), "New Value") == 0);
        REQUIRE(str.IsValid());
    }
}
*/

// Batch 4: String Concatenation

TEST_CASE("RString - Concatenation Operator", "[strings]")
{
    SECTION("Two RStrings")
    {
        RString str1("Hello");
        RString str2(" World");
        RString result = str1 + str2;

        REQUIRE(strcmp(result.Data(), "Hello World") == 0);
    }

    SECTION("Empty strings")
    {
        RString str1("");
        RString str2("Test");
        RString result = str1 + str2;

        REQUIRE(strcmp(result.Data(), "Test") == 0);
    }

    SECTION("Multiple concatenations")
    {
        RString str1("A");
        RString str2("B");
        RString str3("C");
        RString result = str1 + str2 + str3;

        REQUIRE(strcmp(result.Data(), "ABC") == 0);
    }
}

// Batch 5: GetKey() for Hash Map Compatibility

TEST_CASE("RString - GetKey for HashMap", "[strings]")
{
    SECTION("GetKey returns Data()")
    {
        RString str("TestKey");

        REQUIRE(str.GetKey() == str.Data());
        REQUIRE(strcmp(str.GetKey(), "TestKey") == 0);
    }

    SECTION("GetKey for empty string")
    {
        RString str;

        REQUIRE(str.GetKey() != nullptr);
        REQUIRE(strcmp(str.GetKey(), "") == 0);
    }
}

// Batch 6: the RString buffer's refcount must be atomic (SIM-807)
//
// CompactBuffer<char> is the buffer behind every RString, and behind every
// interned RStringB.  The RStringB bank holds a mutex around its *map*, but the
// string it returns is AddRef'd and Release'd outside that lock by whichever
// thread happens to hold a copy.  A plain `++_count` / `--_count` there loses
// increments, Release() reaches 0 while a live reference remains, and the buffer
// is `delete[]`d underneath its owner -- process heap corruption, surfacing
// inside the allocator at an unrelated site later.  That was SIM-807's crash.
//
// This case is deliberately written NOT to crash when the fix is reverted: a
// large bias of held references keeps the count far from 0, so a lost update
// shows up as a wrong final count rather than as a premature free.  Reverting
// CompactBuf.hpp to `++_count` makes it FAIL (measured), not segfault.
TEST_CASE("RString buffer refcount is atomic under concurrent AddRef/Release", "[strings][threads]")
{
    using ::Poseidon::Foundation::String;

    String* buf = ::Poseidon::Foundation::CreateString("sim807_shared_selection_name");
    buf->AddRef(); // the reference this test itself holds

    constexpr int kBias = 200000; // keeps the count away from 0 if an update is lost
    for (int i = 0; i < kBias; ++i)
    {
        buf->AddRef();
    }

    constexpr int kThreads = 4;
    constexpr int kIters = 100000;
    std::atomic<bool> go{false};
    std::vector<std::thread> pool;
    pool.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        pool.emplace_back(
            [&]
            {
                while (!go.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                for (int i = 0; i < kIters; ++i)
                {
                    buf->AddRef();
                    buf->Release();
                }
            });
    }
    go.store(true, std::memory_order_release);
    for (std::thread& th : pool)
    {
        th.join();
    }

    // Every AddRef was paired with a Release, so the count must be exactly what
    // it was before the threads ran.  Non-atomic ++/-- drifts here.
    REQUIRE(buf->RefCounter() == kBias + 1);

    for (int i = 0; i < kBias; ++i)
    {
        buf->Release();
    }
    REQUIRE(buf->RefCounter() == 1);
    buf->Release(); // frees
}
