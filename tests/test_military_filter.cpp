// Tests for the military/government aircraft filter. Dropping these aircraft
// before storage and display is a hard project rule, so every rule type and
// its edge cases are covered here.
#include <gtest/gtest.h>

#include <sstream>

#include "sema/aircraft_registry.hpp"
#include "sema/military_filter.hpp"

using namespace sema;

namespace {

StateVector sv(std::string icao, std::string callsign = "") {
    StateVector s;
    s.icao24 = std::move(icao);
    s.callsign = std::move(callsign);
    return s;
}

MilitaryRules test_rules() {
    return MilitaryRules::from_json(R"({
        "icao_blocks": [
            {"from": "adf7c8", "to": "afffff", "note": "US military"},
            {"from": "4b8f00", "to": "4b8fff", "note": "test block"}
        ],
        "callsign_patterns": ["RCH[0-9]+[A-Z]*", "TUAF[0-9]+[A-Z]*", "NATO[0-9]+[A-Z]*"],
        "operator_keywords": ["air force", "hava kuvvet", "kara kuvvet", "sahil güvenlik",
                              "cumhurbaşkanlığı", "jandarma", "ministry of defen", "navy", "police"],
        "operator_designators": ["GAF"]
    })");
}

AircraftRegistry fixture_registry(const char* name) {
    return AircraftRegistry::load_csv(std::string(SEMA_FIXTURE_DIR) + "/" + name);
}

}  // namespace

// ---------------------------------------------------------------- icao24 parsing

TEST(ParseIcao24, AcceptsSixHexDigitsInAnyCase) {
    EXPECT_EQ(parse_icao24("4b8001"), 0x4b8001u);
    EXPECT_EQ(parse_icao24("ADF7C8"), 0xadf7c8u);
    EXPECT_EQ(parse_icao24("000000"), 0u);
    EXPECT_EQ(parse_icao24("ffffff"), 0xffffffu);
}

TEST(ParseIcao24, RejectsMalformedInput) {
    EXPECT_FALSE(parse_icao24(""));
    EXPECT_FALSE(parse_icao24("4b800"));
    EXPECT_FALSE(parse_icao24("4b80011"));
    EXPECT_FALSE(parse_icao24("4b80g1"));
    EXPECT_FALSE(parse_icao24(" 4b801"));
    EXPECT_FALSE(parse_icao24("~adf7c"));
}

// ---------------------------------------------------------------- icao blocks

TEST(MilitaryFilter, IcaoBlockBoundariesAreInclusive) {
    const MilitaryFilter f(test_rules(), nullptr);
    EXPECT_EQ(f.classify(sv("adf7c8")), ExclusionReason::IcaoBlock);  // first
    EXPECT_EQ(f.classify(sv("afffff")), ExclusionReason::IcaoBlock);  // last
    EXPECT_EQ(f.classify(sv("ae1234")), ExclusionReason::IcaoBlock);  // inside
    EXPECT_FALSE(f.classify(sv("adf7c7")));                           // first - 1
    EXPECT_FALSE(f.classify(sv("b00000")));                           // last + 1
}

TEST(MilitaryFilter, IcaoBlockMatchIsCaseInsensitive) {
    const MilitaryFilter f(test_rules(), nullptr);
    EXPECT_EQ(f.classify(sv("AE1234")), ExclusionReason::IcaoBlock);
    EXPECT_EQ(f.classify(sv("4B8F80")), ExclusionReason::IcaoBlock);
}

TEST(MilitaryFilter, MalformedIcaoDoesNotCrashAndFallsThroughToOtherRules) {
    const MilitaryFilter f(test_rules(), nullptr);
    EXPECT_FALSE(f.classify(sv("zzzzzz")));
    EXPECT_FALSE(f.classify(sv("")));
    EXPECT_EQ(f.classify(sv("zzzzzz", "RCH123")), ExclusionReason::CallsignPattern);
}

TEST(MilitaryRules, RejectsInvalidBlocks) {
    EXPECT_THROW(MilitaryRules::from_json(R"({"icao_blocks":[{"from":"b00000","to":"a00000"}]})"),
                 std::runtime_error);
    EXPECT_THROW(MilitaryRules::from_json(R"({"icao_blocks":[{"from":"xyz","to":"a00000"}]})"),
                 std::runtime_error);
    EXPECT_ANY_THROW(MilitaryRules::from_json("{not json"));
}

TEST(MilitaryRules, MissingSectionsAreEmpty) {
    const auto r = MilitaryRules::from_json("{}");
    EXPECT_TRUE(r.icao_blocks.empty());
    EXPECT_TRUE(r.callsign_patterns.empty());
    const MilitaryFilter f(r, nullptr);
    EXPECT_FALSE(f.classify(sv("ae1234", "RCH1")));
}

// ---------------------------------------------------------------- callsigns

TEST(MilitaryFilter, CallsignPatternsMatch) {
    const MilitaryFilter f(test_rules(), nullptr);
    EXPECT_EQ(f.classify(sv("4b8001", "RCH123")), ExclusionReason::CallsignPattern);
    EXPECT_EQ(f.classify(sv("4b8001", "RCH4A")), ExclusionReason::CallsignPattern);
    EXPECT_EQ(f.classify(sv("4b8001", "TUAF7012")), ExclusionReason::CallsignPattern);
    EXPECT_EQ(f.classify(sv("4b8001", "NATO01")), ExclusionReason::CallsignPattern);
}

TEST(MilitaryFilter, CallsignMatchIsCaseInsensitive) {
    const MilitaryFilter f(test_rules(), nullptr);
    EXPECT_EQ(f.classify(sv("4b8001", "rch123")), ExclusionReason::CallsignPattern);
    EXPECT_EQ(f.classify(sv("4b8001", "tuaf12")), ExclusionReason::CallsignPattern);
}

TEST(MilitaryFilter, CallsignPatternsRequireDigitsAndFullMatch) {
    const MilitaryFilter f(test_rules(), nullptr);
    EXPECT_FALSE(f.classify(sv("4b8001", "RCH")));       // no digits
    EXPECT_FALSE(f.classify(sv("4b8001", "RCHA12")));    // letter right after prefix
    EXPECT_FALSE(f.classify(sv("4b8001", "XRCH123")));   // prefix not at start
    EXPECT_FALSE(f.classify(sv("4b8001", "TUAF")));
    EXPECT_FALSE(f.classify(sv("4b8001", "TUA123")));    // lookalike, not TUAF
    EXPECT_FALSE(f.classify(sv("4b8001", "NATOX1")));
}

TEST(MilitaryFilter, CivilAirlinersPass) {
    const MilitaryFilter f(test_rules(), nullptr);
    for (const char* cs : {"THY1AB", "THY4KM", "PGT123", "AJA4D", "RYR12", "SXS7Q", "UAE55",
                           "DLH1TK", "TKJ12", "AFR1234", "BAW676"}) {
        EXPECT_FALSE(f.classify(sv("4b8001", cs))) << cs;
    }
}

TEST(MilitaryFilter, EmptyCallsignWithCivilAddressPasses) {
    const MilitaryFilter f(test_rules(), nullptr);
    EXPECT_FALSE(f.classify(sv("4b8001", "")));
}

// ---------------------------------------------------------------- registry

TEST(AircraftRegistry, ParsesOldLowercaseHeader) {
    const auto reg = fixture_registry("aircraft_db_sample.csv");
    EXPECT_EQ(reg.size(), 10u);
    const auto* a = reg.find("4b8001");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->registration, "TC-AAA");
    EXPECT_EQ(a->operator_name, "Example Airways");
    EXPECT_EQ(a->operator_icao, "EXA");
    EXPECT_EQ(a->operator_callsign, "EXAMPLE");
    EXPECT_EQ(a->owner, "Example Leasing");
    EXPECT_EQ(a->model, "737-800");
}

TEST(AircraftRegistry, LookupIsCaseInsensitive) {
    const auto reg = fixture_registry("aircraft_db_sample.csv");
    EXPECT_NE(reg.find("4B8001"), nullptr);
    EXPECT_NE(reg.find("4b8005"), nullptr);  // stored upper-case in the CSV
    EXPECT_EQ(reg.find("000000"), nullptr);
}

TEST(AircraftRegistry, ParsesNewQuotedHeaderWithCommasAndEscapedQuotes) {
    const auto reg = fixture_registry("aircraft_db_sample_v2.csv");
    EXPECT_EQ(reg.size(), 4u);
    const auto* a = reg.find("4b9001");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->operator_name, "Sample Hava Taşımacılığı, A.Ş.");
    EXPECT_EQ(a->owner, "Sample, Leasing Ltd");
    EXPECT_EQ(a->operator_icao, "SMQ");
    EXPECT_EQ(a->registration, "TC-BBA");
    EXPECT_EQ(a->model, "A320neo");
    const auto* q = reg.find("4b9003");
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->operator_name, "Example 'Quoted' Air Force");
}

TEST(AircraftRegistry, HandlesDoubleQuotesBomAndCrlf) {
    std::istringstream in("\xEF\xBB\xBF\"icao24\",\"owner\"\r\n\"4b7001\",\"A, B\"\r\n\r\n,\"no address\"\r\n");
    const auto reg = AircraftRegistry::from_csv(in);
    EXPECT_EQ(reg.size(), 1u);
    ASSERT_NE(reg.find("4b7001"), nullptr);
    EXPECT_EQ(reg.find("4b7001")->owner, "A, B");
}

TEST(AircraftRegistry, MissingIcaoColumnThrows) {
    std::istringstream in("registration,owner\nTC-AAA,x\n");
    EXPECT_THROW(AircraftRegistry::from_csv(in), std::runtime_error);
}

TEST(AircraftRegistry, MissingFileThrows) {
    EXPECT_THROW(AircraftRegistry::load_csv("/nonexistent/aircraft.csv"), std::runtime_error);
}

TEST(AircraftRegistry, EmptyInputGivesEmptyRegistry) {
    std::istringstream in("");
    EXPECT_EQ(AircraftRegistry::from_csv(in).size(), 0u);
}

// ---------------------------------------------------------------- operator rules

TEST(MilitaryFilter, OperatorKeywordsInEnglishAndTurkish) {
    const auto reg = fixture_registry("aircraft_db_sample.csv");
    const MilitaryFilter f(test_rules(), &reg);
    EXPECT_EQ(f.classify(sv("4b8002")), ExclusionReason::OperatorKeyword);  // Türk Hava Kuvvetleri
    EXPECT_EQ(f.classify(sv("4b8003")), ExclusionReason::OperatorKeyword);  // TÜRK HAVA KUVVETLERİ
    EXPECT_EQ(f.classify(sv("4b8004")), ExclusionReason::OperatorKeyword);  // Ministry of Defence
    EXPECT_EQ(f.classify(sv("4b8006")), ExclusionReason::OperatorKeyword);  // T.C. CUMHURBAŞKANLIĞI
    EXPECT_EQ(f.classify(sv("4b8007")), ExclusionReason::OperatorKeyword);  // Sahil Güvenlik
    EXPECT_EQ(f.classify(sv("4b8008")), ExclusionReason::OperatorKeyword);  // Jandarma
    EXPECT_EQ(f.classify(sv("4b800a")), ExclusionReason::OperatorKeyword);  // operator callsign NAVY
}

TEST(MilitaryFilter, OperatorKeywordsFromNewFormatRegistry) {
    const auto reg = fixture_registry("aircraft_db_sample_v2.csv");
    const MilitaryFilter f(test_rules(), &reg);
    EXPECT_FALSE(f.classify(sv("4b9001")));  // civil, Turkish characters in name
    EXPECT_EQ(f.classify(sv("4b9002")), ExclusionReason::OperatorKeyword);  // Kara Kuvvetleri
    EXPECT_EQ(f.classify(sv("4b9003")), ExclusionReason::OperatorKeyword);  // ... Air Force
    EXPECT_EQ(f.classify(sv("4b9004")), ExclusionReason::OperatorKeyword);  // ... Police
}

TEST(MilitaryFilter, TurkishCapitalLettersAreFolded) {
    AircraftRegistry reg;
    auto add = [&](std::string icao, std::string op, std::string owner) {
        AircraftInfo a;
        a.icao24 = std::move(icao);
        a.operator_name = std::move(op);
        a.owner = std::move(owner);
        reg.add(std::move(a));
    };
    add("4b7001", "", "HAVA KUVVETLERİ KOMUTANLIĞI");
    add("4b7002", "SAHİL GÜVENLİK", "");
    add("4b7003", "", "CUMHURBAŞKANLIĞI");
    add("4b7004", "", "Jandarma Genel Komutanlığı");
    add("4b7005", "", "KARA KUVVETLERİ");
    const MilitaryFilter f(test_rules(), &reg);
    for (const char* id : {"4b7001", "4b7002", "4b7003", "4b7004", "4b7005"}) {
        EXPECT_EQ(f.classify(sv(id)), ExclusionReason::OperatorKeyword) << id;
    }
}

TEST(MilitaryFilter, OperatorDesignatorMatchIsCaseInsensitive) {
    const auto reg = fixture_registry("aircraft_db_sample.csv");
    const MilitaryFilter f(test_rules(), &reg);
    EXPECT_EQ(f.classify(sv("4b8005")), ExclusionReason::OperatorDesignator);  // "gaf" in CSV
}

TEST(MilitaryFilter, CivilRegistryEntriesAndUnknownAircraftPass) {
    const auto reg = fixture_registry("aircraft_db_sample.csv");
    const MilitaryFilter f(test_rules(), &reg);
    EXPECT_FALSE(f.classify(sv("4b8001", "EXA101")));
    EXPECT_FALSE(f.classify(sv("4b8009", "SMP22")));
    EXPECT_FALSE(f.classify(sv("4bffff", "THY1AB")));  // not in registry
}

TEST(MilitaryFilter, CallsignRuleAppliesEvenWhenRegistrySaysCivil) {
    const auto reg = fixture_registry("aircraft_db_sample.csv");
    const MilitaryFilter f(test_rules(), &reg);
    EXPECT_EQ(f.classify(sv("4b8001", "RCH77")), ExclusionReason::CallsignPattern);
}

TEST(MilitaryFilter, ReasonNames) {
    EXPECT_STREQ(to_string(ExclusionReason::IcaoBlock), "icao_block");
    EXPECT_STREQ(to_string(ExclusionReason::CallsignPattern), "callsign_pattern");
    EXPECT_STREQ(to_string(ExclusionReason::OperatorKeyword), "operator_keyword");
    EXPECT_STREQ(to_string(ExclusionReason::OperatorDesignator), "operator_designator");
}

// ---------------------------------------------------------------- shipped rules

TEST(ShippedRules, LoadAndCompile) {
    const auto rules = MilitaryRules::load(std::string(SEMA_DATA_DIR) + "/military_rules.json");
    EXPECT_FALSE(rules.icao_blocks.empty());
    EXPECT_FALSE(rules.callsign_patterns.empty());
    EXPECT_FALSE(rules.operator_keywords.empty());
    EXPECT_NO_THROW(MilitaryFilter(rules, nullptr));
}

TEST(ShippedRules, DropKnownMilitaryAndKeepCivil) {
    const auto rules = MilitaryRules::load(std::string(SEMA_DATA_DIR) + "/military_rules.json");
    const auto reg = fixture_registry("aircraft_db_sample.csv");
    const MilitaryFilter f(rules, &reg);

    EXPECT_TRUE(f.classify(sv("ae0001")));
    for (const char* cs : {"RCH123", "CNV4421", "TUAF12", "NATO05", "GAF681", "HAF101", "RRR9010"}) {
        EXPECT_TRUE(f.classify(sv("4b0000", cs))) << cs;
    }
    for (const char* id : {"4b8002", "4b8003", "4b8004", "4b8005", "4b8006", "4b8007", "4b8008", "4b800a"}) {
        EXPECT_TRUE(f.classify(sv(id))) << id;
    }

    for (const char* cs : {"THY1AB", "THY4KM", "PGT123", "AJA4D", "RYR12", "SXS7Q", "UAE55", "KKK2",
                           "DLH1TK", "AFR1234", "BAW676", "TKJ12", "QTR8", ""}) {
        EXPECT_FALSE(f.classify(sv("4b8001", cs))) << cs;
    }
    EXPECT_FALSE(f.classify(sv("4b8009", "SMP22")));
}
