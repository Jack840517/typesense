#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <collection_manager.h>
#include "collection.h"

class CollectionOverrideTest : public ::testing::Test {
protected:
    Store *store;
    CollectionManager & collectionManager = CollectionManager::get_instance();
    std::atomic<bool> quit = false;
    Collection *coll_mul_fields;

    void setupCollection() {
        std::string state_dir_path = "/tmp/typesense_test/collection_override";
        LOG(INFO) << "Truncating and creating: " << state_dir_path;
        system(("rm -rf "+state_dir_path+" && mkdir -p "+state_dir_path).c_str());

        store = new Store(state_dir_path);
        collectionManager.init(store, 1.0, "auth_key", quit);
        collectionManager.load(8, 1000);

        std::ifstream infile(std::string(ROOT_DIR)+"test/multi_field_documents.jsonl");
        std::vector<field> fields = {
                field("title", field_types::STRING, false),
                field("starring", field_types::STRING, true),
                field("cast", field_types::STRING_ARRAY, true),
                field("points", field_types::INT32, false)
        };

        coll_mul_fields = collectionManager.get_collection("coll_mul_fields").get();
        if(coll_mul_fields == nullptr) {
            coll_mul_fields = collectionManager.create_collection("coll_mul_fields", 4, fields, "points").get();
        }

        std::string json_line;

        while (std::getline(infile, json_line)) {
            coll_mul_fields->add(json_line);
        }

        infile.close();
    }

    virtual void SetUp() {
        setupCollection();
    }

    virtual void TearDown() {
        collectionManager.drop_collection("coll_mul_fields");
        collectionManager.dispose();
        delete store;
    }
};

TEST_F(CollectionOverrideTest, ExcludeIncludeExactQueryMatch) {
    nlohmann::json override_json = {
            {"id",   "exclude-rule"},
            {
             "rule", {
                             {"query", "of"},
                             {"match", override_t::MATCH_EXACT}
                     }
            }
    };
    override_json["excludes"] = nlohmann::json::array();
    override_json["excludes"][0] = nlohmann::json::object();
    override_json["excludes"][0]["id"] = "4";

    override_json["excludes"][1] = nlohmann::json::object();
    override_json["excludes"][1]["id"] = "11";

    override_t override;
    override_t::parse(override_json, "", override);

    coll_mul_fields->add_override(override);

    std::vector<std::string> facets = {"cast"};

    Option<nlohmann::json> res_op = coll_mul_fields->search("of", {"title"}, "", facets, {}, {0}, 10);
    ASSERT_TRUE(res_op.ok());
    nlohmann::json results = res_op.get();

    ASSERT_EQ(3, results["hits"].size());
    ASSERT_EQ(3, results["found"].get<uint32_t>());
    ASSERT_EQ(6, results["facet_counts"][0]["counts"].size());

    ASSERT_STREQ("12", results["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("5", results["hits"][1]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("17", results["hits"][2]["document"]["id"].get<std::string>().c_str());

    // include
    nlohmann::json override_json_include = {
            {"id",   "include-rule"},
            {
             "rule", {
                             {"query", "in"},
                             {"match", override_t::MATCH_EXACT}
                     }
            }
    };
    override_json_include["includes"] = nlohmann::json::array();
    override_json_include["includes"][0] = nlohmann::json::object();
    override_json_include["includes"][0]["id"] = "0";
    override_json_include["includes"][0]["position"] = 1;

    override_json_include["includes"][1] = nlohmann::json::object();
    override_json_include["includes"][1]["id"] = "3";
    override_json_include["includes"][1]["position"] = 2;

    override_t override_include;
    override_t::parse(override_json_include, "", override_include);

    coll_mul_fields->add_override(override_include);

    res_op = coll_mul_fields->search("in", {"title"}, "", {}, {}, {0}, 10);
    ASSERT_TRUE(res_op.ok());
    results = res_op.get();

    ASSERT_EQ(3, results["hits"].size());
    ASSERT_EQ(3, results["found"].get<uint32_t>());

    ASSERT_STREQ("0", results["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("3", results["hits"][1]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("13", results["hits"][2]["document"]["id"].get<std::string>().c_str());

    // curated results should be marked as such
    ASSERT_EQ(true, results["hits"][0]["curated"].get<bool>());
    ASSERT_EQ(true, results["hits"][1]["curated"].get<bool>());
    ASSERT_EQ(0, results["hits"][2].count("curated"));

    coll_mul_fields->remove_override("exclude-rule");
    coll_mul_fields->remove_override("include-rule");

    // contains cases

    nlohmann::json override_contains_inc = {
            {"id",   "include-rule"},
            {
             "rule", {
                             {"query", "will"},
                             {"match", override_t::MATCH_CONTAINS}
                     }
            }
    };
    override_contains_inc["includes"] = nlohmann::json::array();
    override_contains_inc["includes"][0] = nlohmann::json::object();
    override_contains_inc["includes"][0]["id"] = "0";
    override_contains_inc["includes"][0]["position"] = 1;

    override_contains_inc["includes"][1] = nlohmann::json::object();
    override_contains_inc["includes"][1]["id"] = "1";
    override_contains_inc["includes"][1]["position"] = 7;  // purposely setting it way out

    override_t override_inc_contains;
    override_t::parse(override_contains_inc, "", override_inc_contains);

    coll_mul_fields->add_override(override_inc_contains);

    res_op = coll_mul_fields->search("will smith", {"title"}, "", {}, {}, {0}, 10);
    ASSERT_TRUE(res_op.ok());
    results = res_op.get();

    ASSERT_EQ(4, results["hits"].size());
    ASSERT_EQ(4, results["found"].get<uint32_t>());

    ASSERT_STREQ("0", results["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("3", results["hits"][1]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("2", results["hits"][2]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("1", results["hits"][3]["document"]["id"].get<std::string>().c_str());

    // ability to disable overrides
    bool enable_overrides = false;
    res_op = coll_mul_fields->search("will", {"title"}, "", {}, {}, {0}, 10,
                                     1, FREQUENCY, {false}, 0, spp::sparse_hash_set<std::string>(),
                                     spp::sparse_hash_set<std::string>(), 10, "", 30, 4, "", 0, {}, {}, {}, 0,
                                     "<mark>", "</mark>", {1}, 10000, true, false, enable_overrides);
    ASSERT_TRUE(res_op.ok());
    results = res_op.get();

    ASSERT_EQ(2, results["hits"].size());
    ASSERT_EQ(2, results["found"].get<uint32_t>());

    ASSERT_STREQ("3", results["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("2", results["hits"][1]["document"]["id"].get<std::string>().c_str());

    enable_overrides = true;
    res_op = coll_mul_fields->search("will", {"title"}, "", {}, {}, {0}, 10,
                                     1, FREQUENCY, {false}, 0, spp::sparse_hash_set<std::string>(),
                                     spp::sparse_hash_set<std::string>(), 10, "", 30, 4, "", 0, {}, {}, {}, 0,
                                     "<mark>", "</mark>", {1}, 10000, true, false, enable_overrides);
    ASSERT_TRUE(res_op.ok());
    results = res_op.get();

    ASSERT_EQ(4, results["hits"].size());
    ASSERT_EQ(4, results["found"].get<uint32_t>());

    coll_mul_fields->remove_override("include-rule");
}

TEST_F(CollectionOverrideTest, OverrideJSONValidation) {
    nlohmann::json exclude_json = {
            {"id", "exclude-rule"},
            {
             "rule", {
                       {"query", "of"},
                       {"match", override_t::MATCH_EXACT}
                   }
            }
    };

    exclude_json["excludes"] = nlohmann::json::array();
    exclude_json["excludes"][0] = nlohmann::json::object();
    exclude_json["excludes"][0]["id"] = 11;

    override_t override1;
    auto parse_op = override_t::parse(exclude_json, "", override1);

    ASSERT_FALSE(parse_op.ok());
    ASSERT_STREQ("Exclusion `id` must be a string.", parse_op.error().c_str());

    nlohmann::json include_json = {
            {"id", "include-rule"},
            {
             "rule", {
                           {"query", "of"},
                           {"match", override_t::MATCH_EXACT}
                   }
            }
    };

    include_json["includes"] = nlohmann::json::array();
    include_json["includes"][0] = nlohmann::json::object();
    include_json["includes"][0]["id"] = "11";

    override_t override2;
    parse_op = override_t::parse(include_json, "", override2);

    ASSERT_FALSE(parse_op.ok());
    ASSERT_STREQ("Inclusion definition must define both `id` and `position` keys.", parse_op.error().c_str());

    include_json["includes"][0]["position"] = "1";

    parse_op = override_t::parse(include_json, "", override2);
    ASSERT_FALSE(parse_op.ok());
    ASSERT_STREQ("Inclusion `position` must be an integer.", parse_op.error().c_str());

    include_json["includes"][0]["position"] = 1;
    parse_op = override_t::parse(include_json, "", override2);
    ASSERT_TRUE(parse_op.ok());

    nlohmann::json include_json2 = {
            {"id", "include-rule"},
            {
             "rule", {
                           {"query", "of"},
                           {"match", override_t::MATCH_EXACT}
                   }
            }
    };

    parse_op = override_t::parse(include_json2, "", override2);
    ASSERT_FALSE(parse_op.ok());
    ASSERT_STREQ("Must contain one of:`includes`, `excludes`, `filter_by`.", parse_op.error().c_str());

    include_json2["includes"] = nlohmann::json::array();
    include_json2["includes"][0] = 100;

    parse_op = override_t::parse(include_json2, "", override2);
    ASSERT_FALSE(parse_op.ok());
    ASSERT_STREQ("The `includes` value must be an array of objects.", parse_op.error().c_str());

    nlohmann::json exclude_json2 = {
            {"id", "exclude-rule"},
            {
             "rule", {
                           {"query", "of"},
                           {"match", override_t::MATCH_EXACT}
                   }
            }
    };

    exclude_json2["excludes"] = nlohmann::json::array();
    exclude_json2["excludes"][0] = "100";

    parse_op = override_t::parse(exclude_json2, "", override2);
    ASSERT_FALSE(parse_op.ok());
    ASSERT_STREQ("The `excludes` value must be an array of objects.", parse_op.error().c_str());
}

TEST_F(CollectionOverrideTest, ExcludeIncludeFacetFilterQuery) {
    // Check facet field highlight for overridden results
    nlohmann::json override_json_include = {
        {"id", "include-rule"},
        {
         "rule", {
                   {"query", "not-found"},
                   {"match", override_t::MATCH_EXACT}
               }
        }
    };

    override_json_include["includes"] = nlohmann::json::array();
    override_json_include["includes"][0] = nlohmann::json::object();
    override_json_include["includes"][0]["id"] = "0";
    override_json_include["includes"][0]["position"] = 1;

    override_json_include["includes"][1] = nlohmann::json::object();
    override_json_include["includes"][1]["id"] = "2";
    override_json_include["includes"][1]["position"] = 2;

    override_t override_include;
    override_t::parse(override_json_include, "", override_include);

    coll_mul_fields->add_override(override_include);

    std::map<std::string, override_t> overrides = coll_mul_fields->get_overrides();
    ASSERT_EQ(1, overrides.size());
    auto override_json = overrides["include-rule"].to_json();
    ASSERT_FALSE(override_json.contains("filter_by"));
    ASSERT_FALSE(override_json.contains("remove_matched_tokens"));

    auto results = coll_mul_fields->search("not-found", {"title"}, "", {"starring"}, {}, {0}, 10, 1, FREQUENCY,
                                           {false}, Index::DROP_TOKENS_THRESHOLD,
                                           spp::sparse_hash_set<std::string>(),
                                           spp::sparse_hash_set<std::string>(), 10, "starring: will").get();

    ASSERT_EQ("<mark>Will</mark> Ferrell", results["facet_counts"][0]["counts"][0]["highlighted"].get<std::string>());
    ASSERT_EQ("Will Ferrell", results["facet_counts"][0]["counts"][0]["value"].get<std::string>());
    ASSERT_EQ(1, results["facet_counts"][0]["counts"][0]["count"].get<size_t>());

    coll_mul_fields->remove_override("include-rule");

    // facet count is okay when results are excluded
    nlohmann::json override_json_exclude = {
        {"id",   "exclude-rule"},
        {
         "rule", {
                     {"query", "the"},
                     {"match", override_t::MATCH_EXACT}
                 }
        }
    };
    override_json_exclude["excludes"] = nlohmann::json::array();
    override_json_exclude["excludes"][0] = nlohmann::json::object();
    override_json_exclude["excludes"][0]["id"] = "10";

    override_t override;
    override_t::parse(override_json_exclude, "", override);

    coll_mul_fields->add_override(override);

    results = coll_mul_fields->search("the", {"title"}, "", {"starring"}, {}, {0}, 10, 1, FREQUENCY,
                                      {false}, Index::DROP_TOKENS_THRESHOLD,
                                      spp::sparse_hash_set<std::string>(),
                                      spp::sparse_hash_set<std::string>(), 10, "starring: scott").get();

    ASSERT_EQ(9, results["found"].get<size_t>());

    // "count" would be `2` without exclusion
    ASSERT_EQ("<mark>Scott</mark> Glenn", results["facet_counts"][0]["counts"][0]["highlighted"].get<std::string>());
    ASSERT_EQ(1, results["facet_counts"][0]["counts"][0]["count"].get<size_t>());

    ASSERT_EQ("Kristin <mark>Scott</mark> Thomas", results["facet_counts"][0]["counts"][1]["highlighted"].get<std::string>());
    ASSERT_EQ(1, results["facet_counts"][0]["counts"][1]["count"].get<size_t>());

    // ensure per_page is respected
    // first with per_page = 0
    results = coll_mul_fields->search("the", {"title"}, "", {"starring"}, {}, {0}, 0, 1, FREQUENCY,
                                      {false}, Index::DROP_TOKENS_THRESHOLD,
                                      spp::sparse_hash_set<std::string>(),
                                      spp::sparse_hash_set<std::string>(), 10, "starring: scott").get();

    ASSERT_EQ(9, results["found"].get<size_t>());
    ASSERT_EQ(0, results["hits"].size());

    coll_mul_fields->remove_override("exclude-rule");

    // now with per_page = 1, and an include query

    coll_mul_fields->add_override(override_include);
    results = coll_mul_fields->search("not-found", {"title"}, "", {"starring"}, {}, {0}, 1, 1, FREQUENCY,
                                      {false}, Index::DROP_TOKENS_THRESHOLD,
                                      spp::sparse_hash_set<std::string>(),
                                      spp::sparse_hash_set<std::string>(), 10, "").get();

    ASSERT_EQ(2, results["found"].get<size_t>());
    ASSERT_EQ(1, results["hits"].size());
    ASSERT_EQ("0", results["hits"][0]["document"]["id"].get<std::string>());

    // should be able to replace existing override
    override_include.rule.query = "found";
    coll_mul_fields->add_override(override_include);
    ASSERT_STREQ("found", coll_mul_fields->get_overrides()["include-rule"].rule.query.c_str());

    coll_mul_fields->remove_override("include-rule");
}

TEST_F(CollectionOverrideTest, IncludeExcludeHitsQuery) {
    auto pinned_hits = "13:1,4:2";

    // basic pinning

    auto results = coll_mul_fields->search("the", {"title"}, "", {"starring"}, {}, {0}, 50, 1, FREQUENCY,
                                           {false}, Index::DROP_TOKENS_THRESHOLD,
                                           spp::sparse_hash_set<std::string>(),
                                           spp::sparse_hash_set<std::string>(), 10, "starring: will", 30, 5,
                                           "", 10,
                                           pinned_hits, {}).get();

    ASSERT_EQ(10, results["found"].get<size_t>());
    ASSERT_STREQ("13", results["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("4", results["hits"][1]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("11", results["hits"][2]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("16", results["hits"][3]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("6", results["hits"][4]["document"]["id"].get<std::string>().c_str());

    // both pinning and hiding

    std::string hidden_hits="11,16";
    results = coll_mul_fields->search("the", {"title"}, "", {"starring"}, {}, {0}, 50, 1, FREQUENCY,
                                      {false}, Index::DROP_TOKENS_THRESHOLD,
                                      spp::sparse_hash_set<std::string>(),
                                      spp::sparse_hash_set<std::string>(), 10, "starring: will", 30, 5,
                                      "", 10,
                                      pinned_hits, hidden_hits).get();

    ASSERT_STREQ("13", results["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("4", results["hits"][1]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("6", results["hits"][2]["document"]["id"].get<std::string>().c_str());

    // paginating such that pinned hits appear on second page
    pinned_hits = "13:4,4:5";

    results = coll_mul_fields->search("the", {"title"}, "", {"starring"}, {}, {0}, 2, 2, FREQUENCY,
                                      {false}, Index::DROP_TOKENS_THRESHOLD,
                                      spp::sparse_hash_set<std::string>(),
                                      spp::sparse_hash_set<std::string>(), 10, "starring: will", 30, 5,
                                      "", 10,
                                      pinned_hits, hidden_hits).get();

    ASSERT_STREQ("1", results["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("13", results["hits"][1]["document"]["id"].get<std::string>().c_str());

    // take precedence over override rules

    nlohmann::json override_json_include = {
            {"id", "include-rule"},
            {
             "rule", {
                           {"query", "the"},
                           {"match", override_t::MATCH_EXACT}
                   }
            }
    };

    // trying to include an ID that is also being hidden via `hidden_hits` query param will not work
    // as pinned and hidden hits will take precedence over override rules
    override_json_include["includes"] = nlohmann::json::array();
    override_json_include["includes"][0] = nlohmann::json::object();
    override_json_include["includes"][0]["id"] = "11";
    override_json_include["includes"][0]["position"] = 2;

    override_json_include["includes"][1] = nlohmann::json::object();
    override_json_include["includes"][1]["id"] = "8";
    override_json_include["includes"][1]["position"] = 1;

    override_t override_include;
    override_t::parse(override_json_include, "", override_include);

    coll_mul_fields->add_override(override_include);

    results = coll_mul_fields->search("the", {"title"}, "", {"starring"}, {}, {0}, 50, 1, FREQUENCY,
                                      {false}, Index::DROP_TOKENS_THRESHOLD,
                                      spp::sparse_hash_set<std::string>(),
                                      spp::sparse_hash_set<std::string>(), 10, "starring: will", 30, 5,
                                      "", 10,
                                      {}, {hidden_hits}).get();

    ASSERT_EQ(8, results["found"].get<size_t>());
    ASSERT_STREQ("8", results["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("6", results["hits"][1]["document"]["id"].get<std::string>().c_str());
}

TEST_F(CollectionOverrideTest, PinnedHitsSmallerThanPageSize) {
    auto pinned_hits = "17:1,13:4,11:3";

    // pinned hits larger than page size: check that pagination works

    // without overrides:
    // 11, 16, 6, 8, 1, 0, 10, 4, 13, 17

    auto results = coll_mul_fields->search("the", {"title"}, "", {"starring"}, {}, {0}, 8, 1, FREQUENCY,
                                           {false}, Index::DROP_TOKENS_THRESHOLD,
                                           spp::sparse_hash_set<std::string>(),
                                           spp::sparse_hash_set<std::string>(), 10, "starring: will", 30, 5,
                                           "", 10,
                                           pinned_hits, {}).get();

    std::vector<size_t> expected_ids_p1 = {17, 16, 11, 13, 6, 8, 1, 0};

    ASSERT_EQ(10, results["found"].get<size_t>());
    ASSERT_EQ(8, results["hits"].size());

    for(size_t i=0; i<8; i++) {
        ASSERT_EQ(expected_ids_p1[i], std::stoi(results["hits"][i]["document"]["id"].get<std::string>()));
    }

    std::vector<size_t> expected_ids_p2 = {10, 4};

    results = coll_mul_fields->search("the", {"title"}, "", {"starring"}, {}, {0}, 8, 2, FREQUENCY,
                                      {false}, Index::DROP_TOKENS_THRESHOLD,
                                      spp::sparse_hash_set<std::string>(),
                                      spp::sparse_hash_set<std::string>(), 10, "starring: will", 30, 5,
                                      "", 10,
                                      pinned_hits, {}).get();

    ASSERT_EQ(10, results["found"].get<size_t>());
    ASSERT_EQ(2, results["hits"].size());

    for(size_t i=0; i<2; i++) {
        ASSERT_EQ(expected_ids_p2[i], std::stoi(results["hits"][i]["document"]["id"].get<std::string>()));
    }
}

TEST_F(CollectionOverrideTest, PinnedHitsLargerThanPageSize) {
    auto pinned_hits = "6:1,1:2,16:3,11:4";

    // pinned hits larger than page size: check that pagination works

    auto results = coll_mul_fields->search("the", {"title"}, "", {"starring"}, {}, {0}, 2, 1, FREQUENCY,
                                           {false}, Index::DROP_TOKENS_THRESHOLD,
                                           spp::sparse_hash_set<std::string>(),
                                           spp::sparse_hash_set<std::string>(), 10, "starring: will", 30, 5,
                                           "", 10,
                                           pinned_hits, {}).get();

    ASSERT_EQ(10, results["found"].get<size_t>());
    ASSERT_EQ(2, results["hits"].size());
    ASSERT_STREQ("6", results["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("1", results["hits"][1]["document"]["id"].get<std::string>().c_str());

    results = coll_mul_fields->search("the", {"title"}, "", {"starring"}, {}, {0}, 2, 2, FREQUENCY,
                                      {false}, Index::DROP_TOKENS_THRESHOLD,
                                      spp::sparse_hash_set<std::string>(),
                                      spp::sparse_hash_set<std::string>(), 10, "starring: will", 30, 5,
                                      "", 10,
                                      pinned_hits, {}).get();

    ASSERT_EQ(10, results["found"].get<size_t>());
    ASSERT_EQ(2, results["hits"].size());
    ASSERT_STREQ("16", results["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("11", results["hits"][1]["document"]["id"].get<std::string>().c_str());

    results = coll_mul_fields->search("the", {"title"}, "", {"starring"}, {}, {0}, 2, 3, FREQUENCY,
                                      {false}, Index::DROP_TOKENS_THRESHOLD,
                                      spp::sparse_hash_set<std::string>(),
                                      spp::sparse_hash_set<std::string>(), 10, "starring: will", 30, 5,
                                      "", 10,
                                      pinned_hits, {}).get();

    ASSERT_EQ(10, results["found"].get<size_t>());
    ASSERT_EQ(2, results["hits"].size());
    ASSERT_STREQ("8", results["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("0", results["hits"][1]["document"]["id"].get<std::string>().c_str());
}

TEST_F(CollectionOverrideTest, PinnedHitsWhenThereAreNotEnoughResults) {
    auto pinned_hits = "6:1,1:2,11:5";

    // multiple pinned hits specified, but query produces no result

    auto results = coll_mul_fields->search("notfoundquery", {"title"}, "", {"starring"}, {}, {0}, 10, 1, FREQUENCY,
                                           {false}, Index::DROP_TOKENS_THRESHOLD,
                                           spp::sparse_hash_set<std::string>(),
                                           spp::sparse_hash_set<std::string>(), 10, "starring: will", 30, 5,
                                           "", 10,
                                           pinned_hits, {}).get();

    ASSERT_EQ(3, results["found"].get<size_t>());
    ASSERT_EQ(3, results["hits"].size());
    ASSERT_STREQ("6", results["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("1", results["hits"][1]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("11", results["hits"][2]["document"]["id"].get<std::string>().c_str());

    // multiple pinned hits but only single result
    results = coll_mul_fields->search("burgundy", {"title"}, "", {"starring"}, {}, {0}, 10, 1, FREQUENCY,
                                      {false}, Index::DROP_TOKENS_THRESHOLD,
                                      spp::sparse_hash_set<std::string>(),
                                      spp::sparse_hash_set<std::string>(), 10, "starring: will", 30, 5,
                                      "", 10,
                                      pinned_hits, {}).get();

    ASSERT_EQ(4, results["found"].get<size_t>());
    ASSERT_EQ(4, results["hits"].size());

    ASSERT_STREQ("6", results["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("1", results["hits"][1]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("0", results["hits"][2]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("11", results["hits"][3]["document"]["id"].get<std::string>().c_str());
}

TEST_F(CollectionOverrideTest, HiddenHitsHidingSingleResult) {
    Collection *coll1;

    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("points", field_types::INT32, false),};

    coll1 = collectionManager.get_collection("coll1").get();
    if (coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    std::vector<std::vector<std::string>> records = {
        {"Down There by the Train"}
    };

    for (size_t i = 0; i < records.size(); i++) {
        nlohmann::json doc;

        doc["id"] = std::to_string(i);
        doc["title"] = records[i][0];
        doc["points"] = i;

        ASSERT_TRUE(coll1->add(doc.dump()).ok());
    }

    std::string hidden_hits="0";
    auto results = coll1->search("the train", {"title"}, "", {}, {}, {0}, 50, 1, FREQUENCY,
                                      {false}, Index::DROP_TOKENS_THRESHOLD,
                                      spp::sparse_hash_set<std::string>(),
                                      spp::sparse_hash_set<std::string>(), 10, "", 30, 5,
                                      "", 10,
                                      "", hidden_hits).get();

    ASSERT_EQ(0, results["found"].get<size_t>());
    ASSERT_EQ(0, results["hits"].size());

    results = coll1->search("the train", {"title"}, "points:0", {}, {}, {0}, 50, 1, FREQUENCY,
                           {false}, Index::DROP_TOKENS_THRESHOLD,
                           spp::sparse_hash_set<std::string>(),
                           spp::sparse_hash_set<std::string>(), 10, "", 30, 5,
                           "", 10,
                           "", hidden_hits).get();

    ASSERT_EQ(0, results["found"].get<size_t>());
    ASSERT_EQ(0, results["hits"].size());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionOverrideTest, PinnedHitsGrouping) {
    auto pinned_hits = "6:1,8:1,1:2,13:3,4:3";

    // without any grouping parameter, only the first ID in a position should be picked
    // and other IDs should appear in their original positions

    auto results = coll_mul_fields->search("the", {"title"}, "", {"starring"}, {}, {0}, 50, 1, FREQUENCY,
                                           {false}, Index::DROP_TOKENS_THRESHOLD,
                                           spp::sparse_hash_set<std::string>(),
                                           spp::sparse_hash_set<std::string>(), 10, "starring: will", 30, 5,
                                           "", 10,
                                           pinned_hits, {}).get();

    ASSERT_EQ(10, results["found"].get<size_t>());
    ASSERT_STREQ("6", results["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("1", results["hits"][1]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("13", results["hits"][2]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("11", results["hits"][3]["document"]["id"].get<std::string>().c_str());

    // pinned hits should be marked as curated
    ASSERT_EQ(true, results["hits"][0]["curated"].get<bool>());
    ASSERT_EQ(true, results["hits"][1]["curated"].get<bool>());
    ASSERT_EQ(true, results["hits"][2]["curated"].get<bool>());
    ASSERT_EQ(0, results["hits"][3].count("curated"));

    // with grouping

    results = coll_mul_fields->search("the", {"title"}, "", {"starring"}, {}, {0}, 50, 1, FREQUENCY,
                            {false}, Index::DROP_TOKENS_THRESHOLD,
                            spp::sparse_hash_set<std::string>(),
                            spp::sparse_hash_set<std::string>(), 10, "starring: will", 30, 5,
                            "", 10,
                            pinned_hits, {}, {"cast"}, 2).get();

    ASSERT_EQ(8, results["found"].get<size_t>());

    ASSERT_EQ(1, results["grouped_hits"][0]["group_key"].size());
    ASSERT_EQ(2, results["grouped_hits"][0]["group_key"][0].size());
    ASSERT_STREQ("Chris Evans", results["grouped_hits"][0]["group_key"][0][0].get<std::string>().c_str());
    ASSERT_STREQ("Scarlett Johansson", results["grouped_hits"][0]["group_key"][0][1].get<std::string>().c_str());

    ASSERT_STREQ("6", results["grouped_hits"][0]["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("8", results["grouped_hits"][0]["hits"][1]["document"]["id"].get<std::string>().c_str());

    ASSERT_STREQ("1", results["grouped_hits"][1]["hits"][0]["document"]["id"].get<std::string>().c_str());

    ASSERT_STREQ("13", results["grouped_hits"][2]["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("4", results["grouped_hits"][2]["hits"][1]["document"]["id"].get<std::string>().c_str());

    ASSERT_STREQ("11", results["grouped_hits"][3]["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("16", results["grouped_hits"][4]["hits"][0]["document"]["id"].get<std::string>().c_str());
}

TEST_F(CollectionOverrideTest, PinnedHitsWithWildCardQuery) {
    Collection *coll1;

    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("points", field_types::INT32, false),};

    coll1 = collectionManager.get_collection("coll1").get();
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 3, fields, "points").get();
    }

    size_t num_indexed = 0;

    for(size_t i=0; i<311; i++) {
        nlohmann::json doc;

        doc["id"] = std::to_string(i);
        doc["title"] = "Title " + std::to_string(i);
        doc["points"] = i;

        ASSERT_TRUE(coll1->add(doc.dump()).ok());
        num_indexed++;
    }

    auto pinned_hits = "7:1,4:2";

    auto results = coll1->search("*", {"title"}, "", {}, {}, {0}, 30, 11, FREQUENCY,
                                       {false}, Index::DROP_TOKENS_THRESHOLD,
                                       spp::sparse_hash_set<std::string>(),
                                       spp::sparse_hash_set<std::string>(), 10, "", 30, 5,
                                       "", 10,
                                       pinned_hits, {}, {}, {0}, "", "", {}).get();

    ASSERT_EQ(311, results["found"].get<size_t>());
    ASSERT_EQ(11, results["hits"].size());

    std::vector<size_t> expected_ids = {12, 11, 10, 9, 8, 6, 5, 3, 2, 1, 0};  // 4 and 7 should be missing

    for(size_t i=0; i<11; i++) {
        ASSERT_EQ(expected_ids[i], std::stoi(results["hits"][i]["document"]["id"].get<std::string>()));
    }

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionOverrideTest, PinnedHitsIdsHavingColon) {
    Collection *coll1;

    std::vector<field> fields = {field("url", field_types::STRING, true),
                                 field("points", field_types::INT32, false)};

    std::vector<sort_by> sort_fields = { sort_by("points", "DESC") };

    coll1 = collectionManager.get_collection("coll1").get();
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 4, fields, "points").get();
    }

    for(size_t i=1; i<=10; i++) {
        nlohmann::json doc;
        doc["id"] = std::string("https://example.com/") + std::to_string(i);
        doc["url"] = std::string("https://example.com/") + std::to_string(i);
        doc["points"] = i;

        coll1->add(doc.dump());
    }

    std::vector<std::string> query_fields = {"url"};
    std::vector<std::string> facets;

    std::string pinned_hits_str = "https://example.com/1:1, https://example.com/3:2";  // can have space

    auto res_op = coll1->search("*", {"url"}, "", {}, {}, {0}, 25, 1, FREQUENCY,
                                  {false}, Index::DROP_TOKENS_THRESHOLD,
                                  spp::sparse_hash_set<std::string>(),
                                  spp::sparse_hash_set<std::string>(), 10, "", 30, 5,
                                  "", 10,
                                pinned_hits_str, {});

    ASSERT_TRUE(res_op.ok());

    auto res = res_op.get();

    ASSERT_EQ(10, res["found"].get<size_t>());
    ASSERT_STREQ("https://example.com/1", res["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("https://example.com/3", res["hits"][1]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("https://example.com/10", res["hits"][2]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("https://example.com/9", res["hits"][3]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("https://example.com/2", res["hits"][9]["document"]["id"].get<std::string>().c_str());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionOverrideTest, DynamicFilteringExactMatchBasics) {
    Collection *coll1;

    std::vector<field> fields = {field("name", field_types::STRING, false),
                                 field("category", field_types::STRING, true),
                                 field("brand", field_types::STRING, true),
                                 field("points", field_types::INT32, false)};

    coll1 = collectionManager.get_collection("coll1").get();
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    nlohmann::json doc1;
    doc1["id"] = "0";
    doc1["name"] = "Amazing Shoes";
    doc1["category"] = "shoes";
    doc1["brand"] = "Nike";
    doc1["points"] = 3;

    nlohmann::json doc2;
    doc2["id"] = "1";
    doc2["name"] = "Track Gym";
    doc2["category"] = "shoes";
    doc2["brand"] = "Adidas";
    doc2["points"] = 5;

    nlohmann::json doc3;
    doc3["id"] = "2";
    doc3["name"] = "Running Shoes";
    doc3["category"] = "sports";
    doc3["brand"] = "Nike";
    doc3["points"] = 5;

    ASSERT_TRUE(coll1->add(doc1.dump()).ok());
    ASSERT_TRUE(coll1->add(doc2.dump()).ok());
    ASSERT_TRUE(coll1->add(doc3.dump()).ok());

    std::vector<sort_by> sort_fields = { sort_by("_text_match", "DESC"), sort_by("points", "DESC") };

    auto results = coll1->search("shoes", {"name", "category", "brand"}, "",
                                 {}, sort_fields, {2, 2, 2}, 10).get();

    ASSERT_EQ(3, results["hits"].size());
    ASSERT_EQ("0", results["hits"][0]["document"]["id"].get<std::string>());
    ASSERT_EQ("1", results["hits"][1]["document"]["id"].get<std::string>());
    ASSERT_EQ("2", results["hits"][2]["document"]["id"].get<std::string>());

    // with override, results will be different

    nlohmann::json override_json = {
            {"id",   "dynamic-cat-filter"},
            {
             "rule", {
                         {"query", "{category}"},
                         {"match", override_t::MATCH_EXACT}
                     }
            },
            {"remove_matched_tokens", true},
            {"filter_by", "category: {category}"}
    };

    override_t override;
    auto op = override_t::parse(override_json, "dynamic-cat-filter", override);
    ASSERT_TRUE(op.ok());
    coll1->add_override(override);

    override_json = {
            {"id",   "dynamic-brand-cat-filter"},
            {
             "rule", {
                             {"query", "{brand} {category}"},
                             {"match", override_t::MATCH_EXACT}
                     }
            },
            {"remove_matched_tokens", true},
            {"filter_by", "category: {category} && brand: {brand}"}
    };

    op = override_t::parse(override_json, "dynamic-brand-cat-filter", override);
    ASSERT_TRUE(op.ok());
    coll1->add_override(override);

    results = coll1->search("shoes", {"name", "category", "brand"}, "",
                                       {}, sort_fields, {2, 2, 2}, 10).get();

    ASSERT_EQ(2, results["hits"].size());
    ASSERT_EQ("1", results["hits"][0]["document"]["id"].get<std::string>());
    ASSERT_EQ("0", results["hits"][1]["document"]["id"].get<std::string>());

    ASSERT_EQ("<mark>shoes</mark>", results["hits"][0]["highlights"][0]["snippet"].get<std::string>());
    ASSERT_EQ("<mark>shoes</mark>", results["hits"][1]["highlights"][0]["snippet"].get<std::string>());

    // should not apply filter for non-exact case
    results = coll1->search("running shoes", {"name", "category", "brand"}, "",
                            {}, sort_fields, {2, 2, 2}, 10).get();

    ASSERT_EQ(3, results["hits"].size());

    results = coll1->search("adidas shoes", {"name", "category", "brand"}, "",
                            {}, sort_fields, {2, 2, 2}, 10).get();

    ASSERT_EQ(1, results["hits"].size());
    ASSERT_EQ("1", results["hits"][0]["document"]["id"].get<std::string>());

    // with bad override

    nlohmann::json override_json_bad1 = {
            {"id",   "dynamic-filters-bad1"},
            {
             "rule", {
                         {"query", "{brand}"},
                         {"match", override_t::MATCH_EXACT}
                     }
            },
            {"remove_matched_tokens", true},
            {"filter_by", ""}
    };

    override_t override_bad1;
    op = override_t::parse(override_json_bad1, "dynamic-filters-bad1", override_bad1);
    ASSERT_FALSE(op.ok());
    ASSERT_EQ("The `filter_by` must be a non-empty string.", op.error());

    nlohmann::json override_json_bad2 = {
            {"id",   "dynamic-filters-bad2"},
            {
             "rule", {
                             {"query", "{brand}"},
                             {"match", override_t::MATCH_EXACT}
                     }
            },
            {"remove_matched_tokens", true},
            {"filter_by", {"foo", "bar"}}
    };

    override_t override_bad2;
    op = override_t::parse(override_json_bad2, "dynamic-filters-bad2", override_bad2);
    ASSERT_FALSE(op.ok());
    ASSERT_EQ("The `filter_by` must be a string.", op.error());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionOverrideTest, DynamicFilteringMissingField) {
    Collection *coll1;

    std::vector<field> fields = {field("name", field_types::STRING, false),
                                 field("category", field_types::STRING, true),
                                 field("points", field_types::INT32, false)};

    coll1 = collectionManager.get_collection("coll1").get();
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    nlohmann::json doc1;
    doc1["id"] = "0";
    doc1["name"] = "Amazing Shoes";
    doc1["category"] = "shoes";
    doc1["points"] = 3;

    ASSERT_TRUE(coll1->add(doc1.dump()).ok());

    std::vector<sort_by> sort_fields = { sort_by("_text_match", "DESC"), sort_by("points", "DESC") };

    nlohmann::json override_json = {
            {"id",   "dynamic-cat-filter"},
            {
             "rule", {
                             {"query", "{categories}"},             // this field does NOT exist
                             {"match", override_t::MATCH_EXACT}
                     }
            },
            {"remove_matched_tokens", true},
            {"filter_by", "category: {categories}"}
    };

    override_t override;
    auto op = override_t::parse(override_json, "dynamic-cat-filter", override);
    ASSERT_TRUE(op.ok());
    coll1->add_override(override);

    auto results = coll1->search("shoes", {"name", "category"}, "",
                            {}, sort_fields, {2, 2}, 10).get();

    ASSERT_EQ(1, results["hits"].size());
    ASSERT_EQ("0", results["hits"][0]["document"]["id"].get<std::string>());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionOverrideTest, DynamicFilteringMultiplePlaceholders) {
    Collection* coll1;

    std::vector<field> fields = {field("name", field_types::STRING, false),
                                 field("category", field_types::STRING, true),
                                 field("brand", field_types::STRING, true),
                                 field("color", field_types::STRING, true),
                                 field("points", field_types::INT32, false)};

    coll1 = collectionManager.get_collection("coll1").get();
    if (coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    nlohmann::json doc1;
    doc1["id"] = "0";
    doc1["name"] = "Retro Shoes";
    doc1["category"] = "shoes";
    doc1["color"] = "yellow";
    doc1["brand"] = "Nike Air Jordan";
    doc1["points"] = 3;

    nlohmann::json doc2;
    doc2["id"] = "1";
    doc2["name"] = "Baseball";
    doc2["category"] = "shoes";
    doc2["color"] = "white";
    doc2["brand"] = "Adidas";
    doc2["points"] = 5;

    nlohmann::json doc3;
    doc3["id"] = "2";
    doc3["name"] = "Running Shoes";
    doc3["category"] = "sports";
    doc3["color"] = "grey";
    doc3["brand"] = "Nike";
    doc3["points"] = 5;

    ASSERT_TRUE(coll1->add(doc1.dump()).ok());
    ASSERT_TRUE(coll1->add(doc2.dump()).ok());
    ASSERT_TRUE(coll1->add(doc3.dump()).ok());

    std::vector<sort_by> sort_fields = {sort_by("_text_match", "DESC"), sort_by("points", "DESC")};

    nlohmann::json override_json = {
            {"id",                  "dynamic-cat-filter"},
            {
             "rule",                {
                                            {"query", "{brand} {color} shoes"},
                                            {"match", override_t::MATCH_CONTAINS}
                                    }
            },
            {"remove_matched_tokens", true},
            {"filter_by",           "brand: {brand} && color: {color}"}
    };

    override_t override;
    auto op = override_t::parse(override_json, "dynamic-cat-filter", override);
    ASSERT_TRUE(op.ok());
    coll1->add_override(override);

    // not an exact match of rule (because of "light") so all results will be fetched, not just Air Jordan brand
    auto results = coll1->search("Nike Air Jordan light yellow shoes", {"name", "category", "brand"}, "",
                            {}, sort_fields, {2, 2, 2}, 10).get();

    ASSERT_EQ(3, results["hits"].size());
    ASSERT_EQ("0", results["hits"][0]["document"]["id"].get<std::string>());
    ASSERT_EQ("2", results["hits"][1]["document"]["id"].get<std::string>());
    ASSERT_EQ("1", results["hits"][2]["document"]["id"].get<std::string>());

    // query with tokens at the start that preceding the placeholders in the rule
    results = coll1->search("New Nike Air Jordan yellow shoes", {"name", "category", "brand"}, "",
                            {}, sort_fields, {2, 2, 2}, 10).get();

    ASSERT_EQ(1, results["hits"].size());
    ASSERT_EQ("0", results["hits"][0]["document"]["id"].get<std::string>());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionOverrideTest, DynamicFilteringTokensBetweenPlaceholders) {
    Collection* coll1;

    std::vector<field> fields = {field("name", field_types::STRING, false),
                                 field("category", field_types::STRING, true),
                                 field("brand", field_types::STRING, true),
                                 field("color", field_types::STRING, true),
                                 field("points", field_types::INT32, false)};

    coll1 = collectionManager.get_collection("coll1").get();
    if (coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    nlohmann::json doc1;
    doc1["id"] = "0";
    doc1["name"] = "Retro Shoes";
    doc1["category"] = "shoes";
    doc1["color"] = "yellow";
    doc1["brand"] = "Nike Air Jordan";
    doc1["points"] = 3;

    nlohmann::json doc2;
    doc2["id"] = "1";
    doc2["name"] = "Baseball";
    doc2["category"] = "shoes";
    doc2["color"] = "white";
    doc2["brand"] = "Adidas";
    doc2["points"] = 5;

    nlohmann::json doc3;
    doc3["id"] = "2";
    doc3["name"] = "Running Shoes";
    doc3["category"] = "sports";
    doc3["color"] = "grey";
    doc3["brand"] = "Nike";
    doc3["points"] = 5;

    ASSERT_TRUE(coll1->add(doc1.dump()).ok());
    ASSERT_TRUE(coll1->add(doc2.dump()).ok());
    ASSERT_TRUE(coll1->add(doc3.dump()).ok());

    std::vector<sort_by> sort_fields = {sort_by("_text_match", "DESC"), sort_by("points", "DESC")};

    nlohmann::json override_json = {
            {"id",                  "dynamic-cat-filter"},
            {
             "rule",                {
                                            {"query", "{brand} shoes {color}"},
                                            {"match", override_t::MATCH_CONTAINS}
                                    }
            },
            {"remove_matched_tokens", true},
            {"filter_by",           "brand: {brand} && color: {color}"}
    };

    override_t override;
    auto op = override_t::parse(override_json, "dynamic-cat-filter", override);
    ASSERT_TRUE(op.ok());
    coll1->add_override(override);

    auto results = coll1->search("Nike Air Jordan shoes yellow", {"name", "category", "brand"}, "",
                                 {}, sort_fields, {2, 2, 2}, 10).get();

    ASSERT_EQ(1, results["hits"].size());
    ASSERT_EQ("0", results["hits"][0]["document"]["id"].get<std::string>());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionOverrideTest, DynamicFilteringWithNumericalFilter) {
    Collection* coll1;

    std::vector<field> fields = {field("name", field_types::STRING, false),
                                 field("category", field_types::STRING, true),
                                 field("brand", field_types::STRING, true),
                                 field("color", field_types::STRING, true),
                                 field("points", field_types::INT32, false)};

    coll1 = collectionManager.get_collection("coll1").get();
    if (coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    nlohmann::json doc1;
    doc1["id"] = "0";
    doc1["name"] = "Retro Shoes";
    doc1["category"] = "shoes";
    doc1["color"] = "yellow";
    doc1["brand"] = "Nike";
    doc1["points"] = 15;

    nlohmann::json doc2;
    doc2["id"] = "1";
    doc2["name"] = "Baseball Shoes";
    doc2["category"] = "shoes";
    doc2["color"] = "white";
    doc2["brand"] = "Nike";
    doc2["points"] = 5;

    nlohmann::json doc3;
    doc3["id"] = "2";
    doc3["name"] = "Running Shoes";
    doc3["category"] = "sports";
    doc3["color"] = "grey";
    doc3["brand"] = "Nike";
    doc3["points"] = 5;

    nlohmann::json doc4;
    doc4["id"] = "3";
    doc4["name"] = "Running Shoes";
    doc4["category"] = "sports";
    doc4["color"] = "grey";
    doc4["brand"] = "Adidas";
    doc4["points"] = 5;

    ASSERT_TRUE(coll1->add(doc1.dump()).ok());
    ASSERT_TRUE(coll1->add(doc2.dump()).ok());
    ASSERT_TRUE(coll1->add(doc3.dump()).ok());
    ASSERT_TRUE(coll1->add(doc4.dump()).ok());

    std::vector<sort_by> sort_fields = {sort_by("_text_match", "DESC"), sort_by("points", "DESC")};

    nlohmann::json override_json = {
            {"id",                  "dynamic-cat-filter"},
            {
             "rule",                {
                                            {"query", "popular {brand} shoes"},
                                            {"match", override_t::MATCH_CONTAINS}
                                    }
            },
            {"remove_matched_tokens", false},
            {"filter_by",           "brand: {brand} && points:> 10"}
    };

    override_t override;
    auto op = override_t::parse(override_json, "dynamic-cat-filter", override);
    ASSERT_TRUE(op.ok());

    auto results = coll1->search("popular nike shoes", {"name", "category", "brand"}, "",
                                 {}, sort_fields, {2, 2, 2}, 10).get();
    ASSERT_EQ(4, results["hits"].size());

    coll1->add_override(override);

    results = coll1->search("popular nike shoes", {"name", "category", "brand"}, "",
                                 {}, sort_fields, {2, 2, 2}, 10).get();

    ASSERT_EQ(1, results["hits"].size());
    ASSERT_EQ("0", results["hits"][0]["document"]["id"].get<std::string>());

    // when overrides are disabled

    bool enable_overrides = false;
    results = coll1->search("popular nike shoes", {"name", "category", "brand"}, "",
                            {}, sort_fields, {2, 2, 2}, 10, 1, FREQUENCY, {false, false, false}, 1,
                            spp::sparse_hash_set<std::string>(),
                            spp::sparse_hash_set<std::string>(), 10, "", 30, 4, "", 1, {}, {}, {}, 0,
                            "<mark>", "</mark>", {1, 1, 1}, 10000, true, false, enable_overrides).get();
    ASSERT_EQ(4, results["hits"].size());

    // should not match the defined override

    results = coll1->search("running adidas shoes", {"name", "category", "brand"}, "",
                            {}, sort_fields, {2, 2, 2}, 10).get();

    ASSERT_EQ(4, results["hits"].size());
    ASSERT_EQ("3", results["hits"][0]["document"]["id"].get<std::string>());
    ASSERT_EQ("2", results["hits"][1]["document"]["id"].get<std::string>());
    ASSERT_EQ("0", results["hits"][2]["document"]["id"].get<std::string>());
    ASSERT_EQ("1", results["hits"][3]["document"]["id"].get<std::string>());

    results = coll1->search("adidas", {"name", "category", "brand"}, "",
                            {}, sort_fields, {2, 2, 2}, 10).get();

    ASSERT_EQ(1, results["hits"].size());
    ASSERT_EQ("3", results["hits"][0]["document"]["id"].get<std::string>());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionOverrideTest, DynamicFilteringExactMatch) {
    Collection* coll1;

    std::vector<field> fields = {field("name", field_types::STRING, false),
                                 field("category", field_types::STRING, true),
                                 field("brand", field_types::STRING, true),
                                 field("color", field_types::STRING, true),
                                 field("points", field_types::INT32, false)};

    coll1 = collectionManager.get_collection("coll1").get();
    if (coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    nlohmann::json doc1;
    doc1["id"] = "0";
    doc1["name"] = "Retro Shoes";
    doc1["category"] = "shoes";
    doc1["color"] = "yellow";
    doc1["brand"] = "Nike";
    doc1["points"] = 15;

    nlohmann::json doc2;
    doc2["id"] = "1";
    doc2["name"] = "Baseball Shoes";
    doc2["category"] = "shoes";
    doc2["color"] = "white";
    doc2["brand"] = "Nike";
    doc2["points"] = 5;

    nlohmann::json doc3;
    doc3["id"] = "2";
    doc3["name"] = "Running Shoes";
    doc3["category"] = "sports";
    doc3["color"] = "grey";
    doc3["brand"] = "Nike";
    doc3["points"] = 5;

    nlohmann::json doc4;
    doc4["id"] = "3";
    doc4["name"] = "Running Shoes";
    doc4["category"] = "sports";
    doc4["color"] = "grey";
    doc4["brand"] = "Adidas";
    doc4["points"] = 5;

    ASSERT_TRUE(coll1->add(doc1.dump()).ok());
    ASSERT_TRUE(coll1->add(doc2.dump()).ok());
    ASSERT_TRUE(coll1->add(doc3.dump()).ok());
    ASSERT_TRUE(coll1->add(doc4.dump()).ok());

    std::vector<sort_by> sort_fields = {sort_by("_text_match", "DESC"), sort_by("points", "DESC")};

    nlohmann::json override_json = {
            {"id",                  "dynamic-cat-filter"},
            {
             "rule",                {
                                            {"query", "popular {brand} shoes"},
                                            {"match", override_t::MATCH_EXACT}
                                    }
            },
            {"remove_matched_tokens", false},
            {"filter_by",           "brand: {brand} && points:> 10"}
    };

    override_t override;
    auto op = override_t::parse(override_json, "dynamic-cat-filter", override);
    ASSERT_TRUE(op.ok());

    coll1->add_override(override);

    auto results = coll1->search("really popular nike shoes", {"name", "category", "brand"}, "",
                                  {}, sort_fields, {2, 2, 2}, 10).get();

    ASSERT_EQ(4, results["hits"].size());

    results = coll1->search("popular nike running shoes", {"name", "category", "brand"}, "",
                            {}, sort_fields, {2, 2, 2}, 10).get();

    ASSERT_EQ(4, results["hits"].size());

    results = coll1->search("popular nike shoes running", {"name", "category", "brand"}, "",
                            {}, sort_fields, {2, 2, 2}, 10).get();

    ASSERT_EQ(4, results["hits"].size());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionOverrideTest, DynamicFilteringWithSynonyms) {
    Collection *coll1;

    std::vector<field> fields = {field("name", field_types::STRING, false),
                                 field("category", field_types::STRING, true),
                                 field("brand", field_types::STRING, true),
                                 field("points", field_types::INT32, false)};

    coll1 = collectionManager.get_collection("coll1").get();
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    nlohmann::json doc1;
    doc1["id"] = "0";
    doc1["name"] = "Amazing Shoes";
    doc1["category"] = "shoes";
    doc1["brand"] = "Nike";
    doc1["points"] = 3;

    nlohmann::json doc2;
    doc2["id"] = "1";
    doc2["name"] = "Exciting Track Gym";
    doc2["category"] = "shoes";
    doc2["brand"] = "Adidas";
    doc2["points"] = 5;

    nlohmann::json doc3;
    doc3["id"] = "2";
    doc3["name"] = "Amazing Sneakers";
    doc3["category"] = "sneakers";
    doc3["brand"] = "Adidas";
    doc3["points"] = 4;

    ASSERT_TRUE(coll1->add(doc1.dump()).ok());
    ASSERT_TRUE(coll1->add(doc2.dump()).ok());
    ASSERT_TRUE(coll1->add(doc3.dump()).ok());

    synonym_t synonym1{"sneakers-shoes", {"sneakers"}, {{"shoes"}} };
    synonym_t synonym2{"boots-shoes", {"boots"}, {{"shoes"}} };
    synonym_t synonym3{"exciting-amazing", {"exciting"}, {{"amazing"}} };
    coll1->add_synonym(synonym1);
    coll1->add_synonym(synonym2);
    coll1->add_synonym(synonym3);

    std::vector<sort_by> sort_fields = { sort_by("_text_match", "DESC"), sort_by("points", "DESC") };

    // spaces around field name should still work e.g. "{ field }"
    nlohmann::json override_json1 = {
        {"id",   "dynamic-filters"},
        {
         "rule", {
                     {"query", "{ category }"},
                     {"match", override_t::MATCH_EXACT}
                 }
        },
        {"filter_by", "category: {category}"}
    };

    override_t override1;
    auto op = override_t::parse(override_json1, "dynamic-filters", override1);
    ASSERT_TRUE(op.ok());
    coll1->add_override(override1);

    std::map<std::string, override_t> overrides = coll1->get_overrides();
    ASSERT_EQ(1, overrides.size());
    auto override_json = overrides["dynamic-filters"].to_json();
    ASSERT_EQ("category: {category}", override_json["filter_by"].get<std::string>());
    ASSERT_EQ(true, override_json["remove_matched_tokens"].get<bool>());  // must be true by default

    nlohmann::json override_json2 = {
        {"id",   "static-filters"},
        {
         "rule", {
                     {"query", "exciting"},
                     {"match", override_t::MATCH_CONTAINS}
                 }
        },
        {"remove_matched_tokens", true},
        {"filter_by", "points: [5, 4]"}
    };

    override_t override2;
    op = override_t::parse(override_json2, "static-filters", override2);
    ASSERT_TRUE(op.ok());
    coll1->add_override(override2);

    auto results = coll1->search("sneakers", {"name", "category", "brand"}, "",
                            {}, sort_fields, {2, 2, 2}, 10).get();

    ASSERT_EQ(1, results["hits"].size());
    ASSERT_EQ("2", results["hits"][0]["document"]["id"].get<std::string>());

    // keyword does not exist but has a synonym with results

    results = coll1->search("boots", {"name", "category", "brand"}, "",
                            {}, sort_fields, {2, 2, 2}, 10).get();

    ASSERT_EQ(2, results["hits"].size());

    ASSERT_EQ("1", results["hits"][0]["document"]["id"].get<std::string>());
    ASSERT_EQ("0", results["hits"][1]["document"]["id"].get<std::string>());

    // keyword has no override, but synonym's override is used
    results = coll1->search("exciting", {"name", "category", "brand"}, "",
                            {}, sort_fields, {2, 2, 2}, 10).get();

    ASSERT_EQ(2, results["hits"].size());

    ASSERT_EQ("1", results["hits"][0]["document"]["id"].get<std::string>());
    ASSERT_EQ("2", results["hits"][1]["document"]["id"].get<std::string>());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionOverrideTest, StaticFiltering) {
    Collection *coll1;

    std::vector<field> fields = {field("name", field_types::STRING, false),
                                 field("price", field_types::FLOAT, true),
                                 field("points", field_types::INT32, false)};

    coll1 = collectionManager.get_collection("coll1").get();
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    nlohmann::json doc1;
    doc1["id"] = "0";
    doc1["name"] = "Amazing Shoes";
    doc1["price"] = 399.99;
    doc1["points"] = 3;

    nlohmann::json doc2;
    doc2["id"] = "1";
    doc2["name"] = "Track Shoes";
    doc2["price"] = 49.99;
    doc2["points"] = 5;

    ASSERT_TRUE(coll1->add(doc1.dump()).ok());
    ASSERT_TRUE(coll1->add(doc2.dump()).ok());

    std::vector<sort_by> sort_fields = { sort_by("_text_match", "DESC"), sort_by("points", "DESC") };

    nlohmann::json override_json_contains = {
            {"id",   "static-filters"},
            {
             "rule", {
                             {"query", "expensive"},
                             {"match", override_t::MATCH_CONTAINS}
                     }
            },
            {"remove_matched_tokens", true},
            {"filter_by", "price:> 100"}
    };

    override_t override_contains;
    auto op = override_t::parse(override_json_contains, "static-filters", override_contains);
    ASSERT_TRUE(op.ok());

    coll1->add_override(override_contains);

    nlohmann::json override_json_exact = {
            {"id",   "static-exact-filters"},
            {
             "rule", {
                             {"query", "cheap"},
                             {"match", override_t::MATCH_EXACT}
                     }
            },
            {"remove_matched_tokens", true},
            {"filter_by", "price:< 100"}
    };

    override_t override_exact;
    op = override_t::parse(override_json_exact, "static-exact-filters", override_exact);
    ASSERT_TRUE(op.ok());

    coll1->add_override(override_exact);

    auto results = coll1->search("expensive shoes", {"name"}, "",
                                 {}, sort_fields, {2}, 10, 1, FREQUENCY, {true}, 0).get();

    ASSERT_EQ(1, results["hits"].size());
    ASSERT_EQ("0", results["hits"][0]["document"]["id"].get<std::string>());

    results = coll1->search("expensive", {"name"}, "",
                            {}, sort_fields, {2}, 10, 1, FREQUENCY, {true}, 0).get();

    ASSERT_EQ(1, results["hits"].size());
    ASSERT_EQ("0", results["hits"][0]["document"]["id"].get<std::string>());

    // with synonum for expensive
    synonym_t synonym1{"costly-expensive", {"costly"}, {{"expensive"}} };
    coll1->add_synonym(synonym1);

    results = coll1->search("costly", {"name"}, "",
                            {}, sort_fields, {2}, 10, 1, FREQUENCY, {true}, 0).get();

    ASSERT_EQ(1, results["hits"].size());
    ASSERT_EQ("0", results["hits"][0]["document"]["id"].get<std::string>());

    // with exact match

    results = coll1->search("cheap", {"name"}, "",
                            {}, sort_fields, {2}, 10).get();

    ASSERT_EQ(1, results["hits"].size());
    ASSERT_EQ("1", results["hits"][0]["document"]["id"].get<std::string>());

    // should not work in match contains context

    results = coll1->search("cheap boots", {"name"}, "",
                            {}, sort_fields, {2}, 10).get();

    ASSERT_EQ(0, results["hits"].size());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionOverrideTest, StaticFilterWithAndWithoutQueryStringMutation) {
    Collection *coll1;

    std::vector<field> fields = {field("name", field_types::STRING, false),
                                 field("price", field_types::FLOAT, true),
                                 field("points", field_types::INT32, false)};

    coll1 = collectionManager.get_collection("coll1").get();
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    nlohmann::json doc1;
    doc1["id"] = "0";
    doc1["name"] = "Apple iPad";
    doc1["price"] = 399.99;
    doc1["points"] = 3;

    nlohmann::json doc2;
    doc2["id"] = "1";
    doc2["name"] = "Samsung Charger";
    doc2["price"] = 49.99;
    doc2["points"] = 5;

    nlohmann::json doc3;
    doc3["id"] = "2";
    doc3["name"] = "Samsung Phone";
    doc3["price"] = 249.99;
    doc3["points"] = 5;

    ASSERT_TRUE(coll1->add(doc1.dump()).ok());
    ASSERT_TRUE(coll1->add(doc2.dump()).ok());
    ASSERT_TRUE(coll1->add(doc3.dump()).ok());

    std::vector<sort_by> sort_fields = { sort_by("_text_match", "DESC"), sort_by("points", "DESC") };

    nlohmann::json override_json_contains = {
            {"id",   "static-filters"},
            {
             "rule", {
                             {"query", "apple"},
                             {"match", override_t::MATCH_CONTAINS}
                     }
            },
            {"remove_matched_tokens", false},
            {"filter_by", "price:> 200"}
    };

    override_t override_contains;
    auto op = override_t::parse(override_json_contains, "static-filters", override_contains);
    ASSERT_TRUE(op.ok());

    coll1->add_override(override_contains);

    // first without query string mutation

    auto results = coll1->search("apple", {"name"}, "",
                                 {}, sort_fields, {2}, 10, 1, FREQUENCY, {true}, 0).get();

    ASSERT_EQ(1, results["hits"].size());
    ASSERT_EQ("0", results["hits"][0]["document"]["id"].get<std::string>());

    // now, with query string mutation

    override_json_contains = {
            {"id",   "static-filters"},
            {
             "rule", {
                             {"query", "apple"},
                             {"match", override_t::MATCH_CONTAINS}
                     }
            },
            {"remove_matched_tokens", true},
            {"filter_by", "price:> 200"}
    };

    op = override_t::parse(override_json_contains, "static-filters", override_contains);
    ASSERT_TRUE(op.ok());
    coll1->add_override(override_contains);

    results = coll1->search("apple", {"name"}, "",
                            {}, sort_fields, {2}, 10, 1, FREQUENCY, {true}, 0).get();

    ASSERT_EQ(2, results["hits"].size());
    ASSERT_EQ("2", results["hits"][0]["document"]["id"].get<std::string>());
    ASSERT_EQ("0", results["hits"][1]["document"]["id"].get<std::string>());

    collectionManager.drop_collection("coll1");
}
