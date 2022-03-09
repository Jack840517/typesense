#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <collection_manager.h>
#include "collection.h"

class CollectionSynonymsTest : public ::testing::Test {
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

TEST_F(CollectionSynonymsTest, SynonymParsingFromJson) {
    nlohmann::json syn_json = {
        {"id", "syn-1"},
        {"root", "Ocean"},
        {"synonyms", {"Sea"} }
    };

    synonym_t synonym;
    auto syn_op = synonym_t::parse(syn_json, synonym);
    ASSERT_TRUE(syn_op.ok());

    ASSERT_STREQ("syn-1", synonym.id.c_str());
    ASSERT_STREQ("ocean", synonym.root[0].c_str());
    ASSERT_STREQ("sea", synonym.synonyms[0][0].c_str());

    // should accept without root
    nlohmann::json syn_json_without_root = {
        {"id", "syn-1"},
        {"synonyms", {"Sea", "ocean"} }
    };

    syn_op = synonym_t::parse(syn_json_without_root, synonym);
    ASSERT_TRUE(syn_op.ok());

    // when `id` is not given
    nlohmann::json syn_json_without_id = {
        {"root", "Ocean"},
        {"synonyms", {"Sea"} }
    };

    syn_op = synonym_t::parse(syn_json_without_id, synonym);
    ASSERT_FALSE(syn_op.ok());
    ASSERT_STREQ("Missing `id` field.", syn_op.error().c_str());

    // synonyms missing
    nlohmann::json syn_json_without_synonyms = {
        {"id", "syn-1"},
        {"root", "Ocean"}
    };

    syn_op = synonym_t::parse(syn_json_without_synonyms, synonym);
    ASSERT_FALSE(syn_op.ok());
    ASSERT_STREQ("Could not find an array of `synonyms`", syn_op.error().c_str());

    // synonyms bad type

    nlohmann::json syn_json_bad_type1 = {
        {"id", "syn-1"},
        {"root", "Ocean"},
        {"synonyms", {"Sea", 1} }
    };

    syn_op = synonym_t::parse(syn_json_bad_type1, synonym);
    ASSERT_FALSE(syn_op.ok());
    ASSERT_STREQ("Could not find a valid string array of `synonyms`", syn_op.error().c_str());

    nlohmann::json syn_json_bad_type2 = {
        {"id", "syn-1"},
        {"root", "Ocean"},
        {"synonyms", "foo" }
    };

    syn_op = synonym_t::parse(syn_json_bad_type2, synonym);
    ASSERT_FALSE(syn_op.ok());
    ASSERT_STREQ("Could not find an array of `synonyms`", syn_op.error().c_str());

    nlohmann::json syn_json_bad_type3 = {
        {"id", "syn-1"},
        {"root", "Ocean"},
        {"synonyms", {} }
    };

    syn_op = synonym_t::parse(syn_json_bad_type3, synonym);
    ASSERT_FALSE(syn_op.ok());
    ASSERT_STREQ("Could not find an array of `synonyms`", syn_op.error().c_str());

    // empty string in synonym list
    nlohmann::json syn_json_bad_type4 = {
        {"id", "syn-1"},
        {"root", "Ocean"},
        {"synonyms", {""} }
    };

    syn_op = synonym_t::parse(syn_json_bad_type4, synonym);
    ASSERT_FALSE(syn_op.ok());
    ASSERT_STREQ("Could not find a valid string array of `synonyms`", syn_op.error().c_str());

    // root bad type

    nlohmann::json syn_json_root_bad_type = {
        {"id", "syn-1"},
        {"root", 120},
        {"synonyms", {"Sea"} }
    };

    syn_op = synonym_t::parse(syn_json_root_bad_type, synonym);
    ASSERT_FALSE(syn_op.ok());
    ASSERT_STREQ("Key `root` should be a string.", syn_op.error().c_str());
}

TEST_F(CollectionSynonymsTest, SynonymReductionOneWay) {
    std::vector<std::vector<std::string>> results;

    synonym_t synonym1{"nyc-expansion", {"nyc"}, {{"new", "york"}} };
    coll_mul_fields->add_synonym(synonym1);

    results.clear();
    coll_mul_fields->synonym_reduction({"red", "nyc", "tshirt"}, results);

    ASSERT_EQ(1, results.size());
    ASSERT_EQ(4, results[0].size());

    std::vector<std::string> red_new_york_tshirts = {"red", "new", "york", "tshirt"};
    for(size_t i=0; i<red_new_york_tshirts.size(); i++) {
        ASSERT_STREQ(red_new_york_tshirts[i].c_str(), results[0][i].c_str());
    }

    // when no synonyms exist, reduction should return nothing

    results.clear();
    coll_mul_fields->synonym_reduction({"foo", "bar", "baz"}, results);
    ASSERT_EQ(0, results.size());

    // compression and also ensure that it does not revert back to expansion rule

    results.clear();
    synonym_t synonym2{"new-york-compression", {"new", "york"}, {{"nyc"}} };
    coll_mul_fields->add_synonym(synonym2);

    coll_mul_fields->synonym_reduction({"red", "new", "york", "tshirt"}, results);

    ASSERT_EQ(1, results.size());
    ASSERT_EQ(3, results[0].size());

    std::vector<std::string> red_nyc_tshirts = {"red", "nyc", "tshirt"};
    for(size_t i=0; i<red_nyc_tshirts.size(); i++) {
        ASSERT_STREQ(red_nyc_tshirts[i].c_str(), results[0][i].c_str());
    }

    // replace two synonyms with the same length
    results.clear();
    synonym_t synonym3{"t-shirt-compression", {"t", "shirt"}, {{"tshirt"}} };
    coll_mul_fields->add_synonym(synonym3);

    coll_mul_fields->synonym_reduction({"new", "york", "t", "shirt"}, results);

    ASSERT_EQ(1, results.size());
    ASSERT_EQ(2, results[0].size());

    std::vector<std::string> nyc_tshirt = {"nyc", "tshirt"};
    for(size_t i=0; i<nyc_tshirt.size(); i++) {
        ASSERT_STREQ(nyc_tshirt[i].c_str(), results[0][i].c_str());
    }

    // replace two synonyms with different lengths
    results.clear();
    synonym_t synonym4{"red-crimson", {"red"}, {{"crimson"}} };
    coll_mul_fields->add_synonym(synonym4);

    coll_mul_fields->synonym_reduction({"red", "new", "york", "cap"}, results);

    ASSERT_EQ(1, results.size());
    ASSERT_EQ(3, results[0].size());

    std::vector<std::string> crimson_nyc_cap = {"crimson", "nyc", "cap"};
    for(size_t i=0; i<crimson_nyc_cap.size(); i++) {
        ASSERT_STREQ(crimson_nyc_cap[i].c_str(), results[0][i].c_str());
    }
}

TEST_F(CollectionSynonymsTest, SynonymReductionMultiWay) {
    synonym_t synonym1{"ipod-synonyms", {}, {{"ipod"}, {"i", "pod"}, {"pod"}} };
    coll_mul_fields->add_synonym(synonym1);

    std::vector<std::vector<std::string>> results;
    coll_mul_fields->synonym_reduction({"ipod"}, results);

    ASSERT_EQ(2, results.size());
    ASSERT_EQ(2, results[0].size());
    ASSERT_EQ(1, results[1].size());

    std::vector<std::string> i_pod = {"i", "pod"};
    for(size_t i=0; i<i_pod.size(); i++) {
        ASSERT_STREQ(i_pod[i].c_str(), results[0][i].c_str());
    }

    ASSERT_STREQ("pod", results[1][0].c_str());

    // multiple tokens
    results.clear();
    coll_mul_fields->synonym_reduction({"i", "pod"}, results);

    ASSERT_EQ(2, results.size());
    ASSERT_EQ(1, results[0].size());
    ASSERT_EQ(1, results[1].size());

    ASSERT_STREQ("ipod", results[0][0].c_str());
    ASSERT_STREQ("pod", results[1][0].c_str());

    // multi-token synonym + multi-token synonym definitions
    synonym_t synonym2{"usa-synonyms", {}, {{"usa"}, {"united", "states"}, {"us"},
                                            {"united", "states", "of", "america"}, {"states"}}};
    coll_mul_fields->add_synonym(synonym2);

    results.clear();
    coll_mul_fields->synonym_reduction({"united", "states"}, results);
    ASSERT_EQ(4, results.size());

    ASSERT_EQ(1, results[0].size());
    ASSERT_EQ(1, results[1].size());
    ASSERT_EQ(4, results[2].size());
    ASSERT_EQ(1, results[3].size());

    ASSERT_STREQ("usa", results[0][0].c_str());
    ASSERT_STREQ("us", results[1][0].c_str());

    std::vector<std::string> red_new_york_tshirts = {"united", "states", "of", "america"};
    for(size_t i=0; i<red_new_york_tshirts.size(); i++) {
        ASSERT_STREQ(red_new_york_tshirts[i].c_str(), results[2][i].c_str());
    }

    ASSERT_STREQ("states", results[3][0].c_str());
}

TEST_F(CollectionSynonymsTest, SynonymBelongingToMultipleSets) {
    synonym_t synonym1{"iphone-synonyms", {}, {{"i", "phone"}, {"smart", "phone"}}};
    synonym_t synonym2{"samsung-synonyms", {}, {{"smart", "phone"}, {"galaxy", "phone"}, {"samsung", "phone"}}};
    coll_mul_fields->add_synonym(synonym1);
    coll_mul_fields->add_synonym(synonym2);

    std::vector<std::vector<std::string>> results;
    coll_mul_fields->synonym_reduction({"smart", "phone"}, results);

    ASSERT_EQ(3, results.size());
    ASSERT_EQ(2, results[0].size());
    ASSERT_EQ(2, results[1].size());
    ASSERT_EQ(2, results[2].size());

    ASSERT_STREQ("i", results[0][0].c_str());
    ASSERT_STREQ("phone", results[0][1].c_str());

    ASSERT_STREQ("galaxy", results[1][0].c_str());
    ASSERT_STREQ("phone", results[1][1].c_str());

    ASSERT_STREQ("samsung", results[2][0].c_str());
    ASSERT_STREQ("phone", results[2][1].c_str());
}

TEST_F(CollectionSynonymsTest, OneWaySynonym) {
    nlohmann::json syn_json = {
        {"id", "syn-1"},
        {"root", "Ocean"},
        {"synonyms", {"Sea"} }
    };

    synonym_t synonym;
    auto syn_op = synonym_t::parse(syn_json, synonym);
    ASSERT_TRUE(syn_op.ok());

    // without synonym

    auto res = coll_mul_fields->search("ocean", {"title"}, "", {}, {}, {0}, 10).get();
    ASSERT_EQ(0, res["hits"].size());
    ASSERT_EQ(0, res["found"].get<uint32_t>());

    // add synonym and redo search

    coll_mul_fields->add_synonym(synonym);

    res = coll_mul_fields->search("ocean", {"title"}, "", {}, {}, {0}, 10).get();
    ASSERT_EQ(1, res["hits"].size());
    ASSERT_EQ(1, res["found"].get<uint32_t>());
}

TEST_F(CollectionSynonymsTest, MultiWaySynonym) {
    nlohmann::json syn_json = {
        {"id",       "syn-1"},
        {"synonyms", {"Home Land", "Homeland", "homǝland"}}
    };

    synonym_t synonym;
    auto syn_op = synonym_t::parse(syn_json, synonym);
    ASSERT_TRUE(syn_op.ok());

    // without synonym

    auto res = coll_mul_fields->search("homǝland", {"title"}, "", {}, {}, {0}, 10).get();
    ASSERT_EQ(0, res["hits"].size());
    ASSERT_EQ(0, res["found"].get<uint32_t>());

    coll_mul_fields->add_synonym(synonym);

    res = coll_mul_fields->search("homǝland", {"title"}, "", {}, {}, {0}, 10).get();

    ASSERT_EQ(1, res["hits"].size());
    ASSERT_EQ(1, res["found"].get<uint32_t>());
    ASSERT_STREQ("<mark>Homeland</mark> Security", res["hits"][0]["highlights"][0]["snippet"].get<std::string>().c_str());

    nlohmann::json syn_json2 = {
        {"id",       "syn-2"},
        {"synonyms", {"Samuel L. Jackson", "Sam Jackson", "Leroy"}}
    };

    synonym_t synonym2;
    syn_op = synonym_t::parse(syn_json2, synonym2);
    ASSERT_TRUE(syn_op.ok());

    res = coll_mul_fields->search("samuel leroy jackson", {"starring"}, "", {}, {}, {0}, 10, 1, FREQUENCY, {false}, 0).get();
    ASSERT_EQ(0, res["hits"].size());

    coll_mul_fields->add_synonym(synonym2);

    res = coll_mul_fields->search("samuel leroy jackson", {"starring"}, "", {}, {}, {0}, 10).get();

    ASSERT_EQ(2, res["hits"].size());
    ASSERT_EQ(2, res["found"].get<uint32_t>());
    ASSERT_STREQ("<mark>Samuel</mark> <mark>L.</mark> <mark>Jackson</mark>", res["hits"][0]["highlights"][0]["snippet"].get<std::string>().c_str());
    ASSERT_STREQ("<mark>Samuel</mark> <mark>L.</mark> <mark>Jackson</mark>", res["hits"][1]["highlights"][0]["snippet"].get<std::string>().c_str());

    // for now we don't support synonyms on ANY prefix

    res = coll_mul_fields->search("ler", {"starring"}, "", {}, {}, {0}, 10, 1, FREQUENCY, {true}).get();
    ASSERT_EQ(0, res["hits"].size());
    ASSERT_EQ(0, res["found"].get<uint32_t>());
}

TEST_F(CollectionSynonymsTest, ExactMatchRankedSameAsSynonymMatch) {
    Collection *coll1;

    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("description", field_types::STRING, false),
                                 field("points", field_types::INT32, false),};

    coll1 = collectionManager.get_collection("coll1").get();
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    std::vector<std::vector<std::string>> records = {
        {"Laughing out Loud", "Description 1", "100"},
        {"Stop Laughing", "Description 2", "120"},
        {"LOL sure", "Laughing out loud sure", "200"},
        {"Really ROFL now", "Description 3", "250"},
    };

    for(size_t i=0; i<records.size(); i++) {
        nlohmann::json doc;

        doc["id"] = std::to_string(i);
        doc["title"] = records[i][0];
        doc["description"] = records[i][1];
        doc["points"] = std::stoi(records[i][2]);

        ASSERT_TRUE(coll1->add(doc.dump()).ok());
    }

    nlohmann::json syn_json = {
        {"id",       "syn-1"},
        {"synonyms", {"Lol", "ROFL", "laughing"}}
    };

    synonym_t synonym;
    auto syn_op = synonym_t::parse(syn_json, synonym);
    ASSERT_TRUE(syn_op.ok());

    coll1->add_synonym(synonym);

    auto res = coll1->search("laughing", {"title"}, "", {}, {}, {0}, 10, 1, FREQUENCY, {false}, 0).get();

    ASSERT_EQ(4, res["hits"].size());
    ASSERT_EQ(4, res["found"].get<uint32_t>());

    ASSERT_STREQ("3", res["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("2", res["hits"][1]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("1", res["hits"][2]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("0", res["hits"][3]["document"]["id"].get<std::string>().c_str());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionSynonymsTest, SynonymFieldOrdering) {
    // Synonym match on a field earlier in the fields list should rank above exact match of another field
    Collection *coll1;

    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("description", field_types::STRING, false),
                                 field("points", field_types::INT32, false),};

    coll1 = collectionManager.get_collection("coll1").get();
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }
    std::vector<std::vector<std::string>> records = {
        {"LOL really", "Description 1", "50"},
        {"Never stop", "Description 2", "120"},
        {"Yes and no", "Laughing out loud sure", "100"},
        {"And so on", "Description 3", "250"},
    };

    for(size_t i=0; i<records.size(); i++) {
        nlohmann::json doc;

        doc["id"] = std::to_string(i);
        doc["title"] = records[i][0];
        doc["description"] = records[i][1];
        doc["points"] = std::stoi(records[i][2]);

        ASSERT_TRUE(coll1->add(doc.dump()).ok());
    }

    nlohmann::json syn_json = {
        {"id",       "syn-1"},
        {"synonyms", {"Lol", "ROFL", "laughing"}}
    };

    synonym_t synonym;
    auto syn_op = synonym_t::parse(syn_json, synonym);
    ASSERT_TRUE(syn_op.ok());

    coll1->add_synonym(synonym);

    auto res = coll1->search("laughing", {"title", "description"}, "", {}, {}, {0}, 10, 1, FREQUENCY, {false}, 0).get();

    ASSERT_EQ(2, res["hits"].size());
    ASSERT_EQ(2, res["found"].get<uint32_t>());

    ASSERT_STREQ("0", res["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("2", res["hits"][1]["document"]["id"].get<std::string>().c_str());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionSynonymsTest, DeleteAndUpsertDuplicationOfSynonms) {
    synonym_t synonym1{"ipod-synonyms", {}, {{"ipod"}, {"i", "pod"}, {"pod"}}};
    synonym_t synonym2{"samsung-synonyms", {}, {{"s3"}, {"s3", "phone"}, {"samsung"}}};
    coll_mul_fields->add_synonym(synonym1);
    coll_mul_fields->add_synonym(synonym2);

    ASSERT_EQ(2, coll_mul_fields->get_synonyms().size());
    coll_mul_fields->remove_synonym("ipod-synonyms");

    ASSERT_EQ(1, coll_mul_fields->get_synonyms().size());
    ASSERT_STREQ("samsung-synonyms", coll_mul_fields->get_synonyms()["samsung-synonyms"].id.c_str());

    // try to upsert synonym with same ID

    synonym2.root = {"s3", "smartphone"};
    auto upsert_op = coll_mul_fields->add_synonym(synonym2);
    ASSERT_TRUE(upsert_op.ok());

    ASSERT_EQ(1, coll_mul_fields->get_synonyms().size());

    synonym_t synonym2_updated;
    coll_mul_fields->get_synonym(synonym2.id, synonym2_updated);

    ASSERT_EQ("s3", synonym2_updated.root[0]);
    ASSERT_EQ("smartphone", synonym2_updated.root[1]);

    coll_mul_fields->remove_synonym("samsung-synonyms");
    ASSERT_EQ(0, coll_mul_fields->get_synonyms().size());
}

TEST_F(CollectionSynonymsTest, SynonymJsonSerialization) {
    synonym_t synonym1{"ipod-synonyms", {"apple", "ipod"}, {{"ipod"}, {"i", "pod"}, {"pod"}}};
    nlohmann::json obj = synonym1.to_view_json();
    ASSERT_STREQ("ipod-synonyms", obj["id"].get<std::string>().c_str());
    ASSERT_STREQ("apple ipod", obj["root"].get<std::string>().c_str());

    ASSERT_EQ(3, obj["synonyms"].size());
    ASSERT_STREQ("ipod", obj["synonyms"][0].get<std::string>().c_str());
    ASSERT_STREQ("i pod", obj["synonyms"][1].get<std::string>().c_str());
    ASSERT_STREQ("pod", obj["synonyms"][2].get<std::string>().c_str());
}

TEST_F(CollectionSynonymsTest, SynonymSingleTokenExactMatch) {
    Collection *coll1;

    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("description", field_types::STRING, false),
                                 field("points", field_types::INT32, false),};

    coll1 = collectionManager.get_collection("coll1").get();
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    std::vector<std::vector<std::string>> records = {
        {"Smashed Lemon", "Description 1", "100"},
        {"Lulu Guinness", "Description 2", "100"},
        {"Lululemon", "Description 3", "100"},
    };

    for(size_t i=0; i<records.size(); i++) {
        nlohmann::json doc;

        doc["id"] = std::to_string(i);
        doc["title"] = records[i][0];
        doc["description"] = records[i][1];
        doc["points"] = std::stoi(records[i][2]);

        ASSERT_TRUE(coll1->add(doc.dump()).ok());
    }

    synonym_t synonym1{"syn-1", {"lulu", "lemon"}, {{"lululemon"}}};
    coll1->add_synonym(synonym1);

    auto res = coll1->search("lulu lemon", {"title"}, "", {}, {}, {2}, 10, 1, FREQUENCY, {true}, 1).get();

    ASSERT_EQ(2, res["hits"].size());
    ASSERT_EQ(2, res["found"].get<uint32_t>());

    ASSERT_STREQ("2", res["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("1", res["hits"][1]["document"]["id"].get<std::string>().c_str());

    collectionManager.drop_collection("coll1");
}