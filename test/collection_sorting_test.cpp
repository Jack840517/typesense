#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <collection_manager.h>
#include "collection.h"

class CollectionSortingTest : public ::testing::Test {
protected:
    Store *store;
    CollectionManager & collectionManager = CollectionManager::get_instance();
    std::atomic<bool> quit = false;

    std::vector<std::string> query_fields;
    std::vector<sort_by> sort_fields;

    void setupCollection() {
        std::string state_dir_path = "/tmp/typesense_test/collection_sorting";
        LOG(INFO) << "Truncating and creating: " << state_dir_path;
        system(("rm -rf "+state_dir_path+" && mkdir -p "+state_dir_path).c_str());

        store = new Store(state_dir_path);
        collectionManager.init(store, 1.0, "auth_key", quit);
        collectionManager.load(8, 1000);
    }

    virtual void SetUp() {
        setupCollection();
    }

    virtual void TearDown() {
        collectionManager.dispose();
        delete store;
    }
};

TEST_F(CollectionSortingTest, SortingOrder) {
    Collection *coll_mul_fields;

    std::ifstream infile(std::string(ROOT_DIR)+"test/multi_field_documents.jsonl");
    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("starring", field_types::STRING, false),
                                 field("points", field_types::INT32, false),
                                 field("cast", field_types::STRING_ARRAY, false)};

    coll_mul_fields = collectionManager.get_collection("coll_mul_fields").get();
    if(coll_mul_fields == nullptr) {
        coll_mul_fields = collectionManager.create_collection("coll_mul_fields", 4, fields, "points").get();
    }

    std::string json_line;

    while (std::getline(infile, json_line)) {
        coll_mul_fields->add(json_line);
    }

    infile.close();

    query_fields = {"title"};
    std::vector<std::string> facets;
    sort_fields = { sort_by("points", "ASC") };
    nlohmann::json results = coll_mul_fields->search("the", query_fields, "", facets, sort_fields, {0}, 15, 1, FREQUENCY, {false}).get();
    ASSERT_EQ(10, results["hits"].size());

    std::vector<std::string> ids = {"17", "13", "10", "4", "0", "1", "8", "6", "16", "11"};

    for(size_t i = 0; i < results["hits"].size(); i++) {
        nlohmann::json result = results["hits"].at(i);
        std::string result_id = result["document"]["id"];
        std::string id = ids.at(i);
        ASSERT_STREQ(id.c_str(), result_id.c_str());
    }

    // limiting results to just 5, "ASC" keyword must be case insensitive
    sort_fields = { sort_by("points", "asc") };
    results = coll_mul_fields->search("the", query_fields, "", facets, sort_fields, {0}, 5, 1, FREQUENCY, {false}).get();
    ASSERT_EQ(5, results["hits"].size());

    ids = {"17", "13", "10", "4", "0"};

    for(size_t i = 0; i < results["hits"].size(); i++) {
        nlohmann::json result = results["hits"].at(i);
        std::string result_id = result["document"]["id"];
        std::string id = ids.at(i);
        ASSERT_STREQ(id.c_str(), result_id.c_str());
    }

    // desc

    sort_fields = { sort_by("points", "dEsc") };
    results = coll_mul_fields->search("the", query_fields, "", facets, sort_fields, {0}, 15, 1, FREQUENCY, {false}).get();
    ASSERT_EQ(10, results["hits"].size());

    ids = {"11", "16", "6", "8", "1", "0", "10", "4", "13", "17"};

    for(size_t i = 0; i < results["hits"].size(); i++) {
        nlohmann::json result = results["hits"].at(i);
        std::string result_id = result["document"]["id"];
        std::string id = ids.at(i);
        ASSERT_STREQ(id.c_str(), result_id.c_str());
    }

    // With empty list of sort_by fields:
    // should be ordered desc on the default sorting field, since the match score will be the same for all records.
    sort_fields = { };
    results = coll_mul_fields->search("of", query_fields, "", facets, sort_fields, {0}, 10, 1, FREQUENCY, {false}).get();
    ASSERT_EQ(5, results["hits"].size());

    ids = {"11", "12", "5", "4", "17"};

    for(size_t i = 0; i < results["hits"].size(); i++) {
        nlohmann::json result = results["hits"].at(i);
        std::string result_id = result["document"]["id"];
        std::string id = ids.at(i);
        ASSERT_STREQ(id.c_str(), result_id.c_str());
    }

    collectionManager.drop_collection("coll_mul_fields");
}

TEST_F(CollectionSortingTest, DefaultSortingFieldValidations) {
    // Default sorting field must be a  numerical field
    std::vector<field> fields = {field("name", field_types::STRING, false),
                                 field("tags", field_types::STRING_ARRAY, true),
                                 field("age", field_types::INT32, false),
                                 field("average", field_types::INT32, false) };

    std::vector<sort_by> sort_fields = { sort_by("age", "DESC"), sort_by("average", "DESC") };

    Option<Collection*> collection_op = collectionManager.create_collection("sample_collection", 4, fields, "name");
    ASSERT_FALSE(collection_op.ok());
    ASSERT_EQ("Default sorting field `name` must be a single valued numerical field.", collection_op.error());
    collectionManager.drop_collection("sample_collection");

    // Default sorting field must exist as a field in schema

    sort_fields = { sort_by("age", "DESC"), sort_by("average", "DESC") };
    collection_op = collectionManager.create_collection("sample_collection", 4, fields, "NOT-DEFINED");
    ASSERT_FALSE(collection_op.ok());
    ASSERT_EQ("Default sorting field is defined as `NOT-DEFINED` but is not found in the schema.", collection_op.error());
    collectionManager.drop_collection("sample_collection");
}

TEST_F(CollectionSortingTest, NoDefaultSortingField) {
    Collection *coll1;

    std::ifstream infile(std::string(ROOT_DIR)+"test/documents.jsonl");
    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("points", field_types::INT32, false)};

    coll1 = collectionManager.get_collection("coll1").get();
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 4, fields).get();
    }

    std::string json_line;

    while (std::getline(infile, json_line)) {
        coll1->add(json_line);
    }

    infile.close();

    // without a default sorting field, matches should be sorted by (text_match, seq_id)
    auto results = coll1->search("rocket", {"title"}, "", {}, {}, {1}, 10, 1, FREQUENCY, {false}).get();

    ASSERT_EQ(4, results["found"].get<size_t>());
    ASSERT_EQ(4, results["hits"].size());
    ASSERT_EQ(24, results["out_of"]);

    std::vector<std::string> ids = {"16", "15", "7", "0"};

    for(size_t i=0; i < results["hits"].size(); i++) {
        ASSERT_EQ(ids[i], results["hits"][i]["document"]["id"].get<std::string>());
    }

    // try removing a document and doing wildcard (tests the seq_id array used for wildcard searches)
    auto remove_op = coll1->remove("0");
    ASSERT_TRUE(remove_op.ok());

    results = coll1->search("*", {}, "", {}, {}, {1}, 30, 1, FREQUENCY, {false}).get();

    ASSERT_EQ(23, results["found"].get<size_t>());
    ASSERT_EQ(23, results["hits"].size());
    ASSERT_EQ(23, results["out_of"]);

    for(size_t i=23; i >= 1; i--) {
        std::string doc_id = (i == 4) ? "foo" : std::to_string(i);
        ASSERT_EQ(doc_id, results["hits"][23 - i]["document"]["id"].get<std::string>());
    }
}

TEST_F(CollectionSortingTest, FrequencyOrderedTokensWithoutDefaultSortingField) {
    // when no default sorting field is provided, tokens must be ordered on frequency
    Collection *coll1;
    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("points", field_types::INT32, false)};

    coll1 = collectionManager.get_collection("coll1").get();
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields).get();
    }

    // since only top 4 tokens are fetched for prefixes, the "enyzme" should not show up in the results
    std::vector<std::string> tokens = {
        "enter", "elephant", "enamel", "ercot", "enyzme", "energy",
        "epoch", "epyc", "express", "everest", "end"
    };

    for(size_t i = 0; i < tokens.size(); i++) {
        size_t num_repeat = tokens.size() - i;

        std::string title = tokens[i];

        for(size_t j = 0; j < num_repeat; j++) {
            nlohmann::json doc;
            doc["title"] = title;
            doc["points"] = num_repeat;
            coll1->add(doc.dump());
        }
    }

    auto results = coll1->search("e", {"title"}, "", {}, {}, {0}, 100, 1, NOT_SET, {true}).get();

    // [11 + 10 + 9 + 8] + 7 + 6 + 5 + 4 + 3 + 2
    ASSERT_EQ(38, results["found"].get<size_t>());

    // we have to ensure that no result contains the word "end" since it occurs least number of times
    bool found_end = false;
    for(auto& res: results["hits"].items()) {
        if(res.value()["document"]["title"] == "enyzme") {
            found_end = true;
        }
    }

    ASSERT_FALSE(found_end);
}

TEST_F(CollectionSortingTest, Int64AsDefaultSortingField) {
    Collection *coll_mul_fields;

    std::ifstream infile(std::string(ROOT_DIR)+"test/multi_field_documents.jsonl");
    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("starring", field_types::STRING, false),
                                 field("points", field_types::INT64, false),
                                 field("cast", field_types::STRING_ARRAY, false)};

    coll_mul_fields = collectionManager.get_collection("coll_mul_fields").get();
    if(coll_mul_fields == nullptr) {
        coll_mul_fields = collectionManager.create_collection("coll_mul_fields", 4, fields, "points").get();
    }

    auto doc_str1 = "{\"title\": \"foo\", \"starring\": \"bar\", \"points\": 343234324234233234, \"cast\": [\"baz\"] }";
    const Option<nlohmann::json> & add_op = coll_mul_fields->add(doc_str1);
    ASSERT_TRUE(add_op.ok());

    auto doc_str2 = "{\"title\": \"foo\", \"starring\": \"bar\", \"points\": 343234324234233232, \"cast\": [\"baz\"] }";
    auto doc_str3 = "{\"title\": \"foo\", \"starring\": \"bar\", \"points\": 343234324234233235, \"cast\": [\"baz\"] }";
    auto doc_str4 = "{\"title\": \"foo\", \"starring\": \"bar\", \"points\": 343234324234233231, \"cast\": [\"baz\"] }";

    coll_mul_fields->add(doc_str2);
    coll_mul_fields->add(doc_str3);
    coll_mul_fields->add(doc_str4);

    query_fields = {"title"};
    std::vector<std::string> facets;
    sort_fields = { sort_by("points", "ASC") };
    nlohmann::json results = coll_mul_fields->search("foo", query_fields, "", facets, sort_fields, {0}, 10, 1, FREQUENCY, {false}).get();
    ASSERT_EQ(4, results["hits"].size());

    std::vector<std::string> ids = {"3", "1", "0", "2"};

    for(size_t i = 0; i < results["hits"].size(); i++) {
        nlohmann::json result = results["hits"].at(i);
        std::string result_id = result["document"]["id"];
        std::string id = ids.at(i);
        ASSERT_STREQ(id.c_str(), result_id.c_str());
    }

    // DESC
    sort_fields = { sort_by("points", "desc") };
    results = coll_mul_fields->search("foo", query_fields, "", facets, sort_fields, {0}, 10, 1, FREQUENCY, {false}).get();
    ASSERT_EQ(4, results["hits"].size());

    ids = {"2", "0", "1", "3"};

    for(size_t i = 0; i < results["hits"].size(); i++) {
        nlohmann::json result = results["hits"].at(i);
        std::string result_id = result["document"]["id"];
        std::string id = ids.at(i);
        ASSERT_STREQ(id.c_str(), result_id.c_str());
    }
}

TEST_F(CollectionSortingTest, SortOnFloatFields) {
    Collection *coll_float_fields;

    std::ifstream infile(std::string(ROOT_DIR)+"test/float_documents.jsonl");
    std::vector<field> fields = {
            field("title", field_types::STRING, false),
            field("score", field_types::FLOAT, false),
            field("average", field_types::FLOAT, false)
    };

    std::vector<sort_by> sort_fields_desc = { sort_by("score", "DESC"), sort_by("average", "DESC") };

    coll_float_fields = collectionManager.get_collection("coll_float_fields").get();
    if(coll_float_fields == nullptr) {
        coll_float_fields = collectionManager.create_collection("coll_float_fields", 4, fields, "score").get();
    }

    std::string json_line;

    while (std::getline(infile, json_line)) {
        coll_float_fields->add(json_line);
    }

    infile.close();

    query_fields = {"title"};
    std::vector<std::string> facets;
    nlohmann::json results = coll_float_fields->search("Jeremy", query_fields, "", facets, sort_fields_desc, {0}, 10, 1, FREQUENCY, {false}).get();
    ASSERT_EQ(7, results["hits"].size());

    std::vector<std::string> ids = {"2", "0", "3", "1", "5", "4", "6"};

    for(size_t i = 0; i < results["hits"].size(); i++) {
        nlohmann::json result = results["hits"].at(i);
        std::string result_id = result["document"]["id"];
        std::string id = ids.at(i);
        ASSERT_STREQ(id.c_str(), result_id.c_str());
    }

    std::vector<sort_by> sort_fields_asc = { sort_by("score", "ASC"), sort_by("average", "ASC") };
    results = coll_float_fields->search("Jeremy", query_fields, "", facets, sort_fields_asc, {0}, 10, 1, FREQUENCY, {false}).get();
    ASSERT_EQ(7, results["hits"].size());

    ids = {"6", "4", "5", "1", "3", "0", "2"};

    for(size_t i = 0; i < results["hits"].size(); i++) {
        nlohmann::json result = results["hits"].at(i);
        std::string result_id = result["document"]["id"];
        std::string id = ids.at(i);
        EXPECT_STREQ(id.c_str(), result_id.c_str());
    }

    // second field by desc

    std::vector<sort_by> sort_fields_asc_desc = { sort_by("score", "ASC"), sort_by("average", "DESC") };
    results = coll_float_fields->search("Jeremy", query_fields, "", facets, sort_fields_asc_desc, {0}, 10, 1, FREQUENCY, {false}).get();
    ASSERT_EQ(7, results["hits"].size());

    ids = {"5", "4", "6", "1", "3", "0", "2"};

    for(size_t i = 0; i < results["hits"].size(); i++) {
        nlohmann::json result = results["hits"].at(i);
        std::string result_id = result["document"]["id"];
        std::string id = ids.at(i);
        EXPECT_STREQ(id.c_str(), result_id.c_str());
    }

    collectionManager.drop_collection("coll_float_fields");
}

TEST_F(CollectionSortingTest, ThreeSortFieldsLimit) {
    Collection *coll1;

    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("points", field_types::INT32, false),
                                 field("average", field_types::INT32, false),
                                 field("min", field_types::INT32, false),
                                 field("max", field_types::INT32, false),
                                 };

    coll1 = collectionManager.get_collection("coll1").get();
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 4, fields, "points").get();
    }

    nlohmann::json doc1;

    doc1["id"] = "100";
    doc1["title"] = "The quick brown fox";
    doc1["points"] = 25;
    doc1["average"] = 25;
    doc1["min"] = 25;
    doc1["max"] = 25;

    coll1->add(doc1.dump());

    std::vector<sort_by> sort_fields_desc = {
        sort_by("points", "DESC"),
        sort_by("average", "DESC"),
        sort_by("max", "DESC"),
        sort_by("min", "DESC"),
    };

    query_fields = {"title"};
    auto res_op = coll1->search("the", query_fields, "", {}, sort_fields_desc, {0}, 10, 1, FREQUENCY, {false});

    ASSERT_FALSE(res_op.ok());
    ASSERT_STREQ("Only upto 3 sort_by fields can be specified.", res_op.error().c_str());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionSortingTest, ThreeSortFieldsTextMatchLast) {
    Collection *coll1;

    std::vector<field> fields = { field("title", field_types::STRING, false),
                                 field("artist", field_types::STRING, false),
                                 field("popularity", field_types::INT32, false),
                                 field("points", field_types::INT32, false),};

    coll1 = collectionManager.get_collection("coll1").get();
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    std::vector<std::vector<std::string>> records = {
        {"Coby Grant", "100"},    // text_match: 33684577
        {"Coby Prant", "84642"},  // text_match: 129377
    };

    for(size_t i=0; i<records.size(); i++) {
        nlohmann::json doc;

        doc["id"] = std::to_string(i);
        doc["title"] = records[i][0];
        doc["artist"] = records[i][0];
        doc["popularity"] = std::stoi(records[i][1]);
        doc["points"] = i;

        ASSERT_TRUE(coll1->add(doc.dump()).ok());
    }

    std::vector<sort_by> sort_fields = { sort_by("popularity", "DESC"), sort_by("points", "DESC"), sort_by(sort_field_const::text_match, "DESC") };

    auto res = coll1->search("grant",
                             {"title","artist"}, "", {}, sort_fields, {1}, 10, 1, FREQUENCY, {false}, 10,
                             spp::sparse_hash_set<std::string>(),
                             spp::sparse_hash_set<std::string>(), 10, "", 30, 5,
                             "", 10).get();

    ASSERT_EQ(2, res["found"].get<size_t>());
    ASSERT_STREQ("1", res["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("0", res["hits"][1]["document"]["id"].get<std::string>().c_str());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionSortingTest, SingleFieldTextMatchScoreDefault) {
    // when queried with a single field, _text_match score should be used implicitly as the second sorting field
    Collection *coll1;

    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("points", field_types::INT32, false),};

    coll1 = collectionManager.get_collection("coll1").get();
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    std::vector<std::vector<std::string>> records = {
        {"Alppha Beta"},
        {"Alpha Beta"},
        {"Alphas Beta"},
    };

    for(size_t i=0; i<records.size(); i++) {
        nlohmann::json doc;

        doc["id"] = std::to_string(i);
        doc["title"] = records[i][0];
        doc["points"] = 100;

        ASSERT_TRUE(coll1->add(doc.dump()).ok());
    }

    std::vector<sort_by> sort_fields = { sort_by("points", "DESC") };

    auto results = coll1->search("alpha",
                                 {"title"}, "", {}, sort_fields, {2}, 10, 1, FREQUENCY,
                                 {false}, 10,
                                 spp::sparse_hash_set<std::string>(),
                                 spp::sparse_hash_set<std::string>(), 10, "", 30, 5,
                                 "", 10).get();

    ASSERT_EQ(3, results["found"].get<size_t>());
    ASSERT_EQ(3, results["hits"].size());

    ASSERT_STREQ("1", results["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("2", results["hits"][1]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("0", results["hits"][2]["document"]["id"].get<std::string>().c_str());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionSortingTest, NegativeInt64Value) {
    Collection *coll1;

    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("points", field_types::INT64, false),
    };

    coll1 = collectionManager.get_collection("coll1").get();
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 4, fields, "points").get();
    }

    nlohmann::json doc1;

    doc1["id"] = "100";
    doc1["title"] = "The quick brown fox";
    doc1["points"] = -2678400;

    coll1->add(doc1.dump());

    std::vector<sort_by> sort_fields_desc = {
      sort_by("points", "DESC")
    };

    query_fields = {"title"};
    auto res = coll1->search("*", query_fields, "points:>=1577836800", {}, sort_fields_desc, {0}, 10, 1, FREQUENCY,
                             {false}).get();

    ASSERT_EQ(0, res["found"].get<size_t>());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionSortingTest, GeoPointSorting) {
    Collection *coll1;

    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("loc", field_types::GEOPOINT, false),
                                 field("points", field_types::INT32, false),};

    coll1 = collectionManager.get_collection("coll1").get();
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    std::vector<std::vector<std::string>> records = {
        {"Palais Garnier", "48.872576479306765, 2.332291112241466"},
        {"Sacre Coeur", "48.888286721920934, 2.342340862419206"},
        {"Arc de Triomphe", "48.87538726829884, 2.296113163780903"},
        {"Place de la Concorde", "48.86536119187326, 2.321850747347093"},
        {"Louvre Musuem", "48.86065813197502, 2.3381285349616725"},
        {"Les Invalides", "48.856648379569904, 2.3118555692631357"},
        {"Eiffel Tower", "48.85821022164442, 2.294239067890161"},
        {"Notre-Dame de Paris", "48.852455825574495, 2.35071182406452"},
        {"Musee Grevin", "48.872370541246816, 2.3431536410008906"},
        {"Pantheon", "48.84620987789056, 2.345152755563131"},
    };

    for(size_t i=0; i<records.size(); i++) {
        nlohmann::json doc;

        std::vector<std::string> lat_lng;
        StringUtils::split(records[i][1], lat_lng, ", ");

        double lat = std::stod(lat_lng[0]);
        double lng = std::stod(lat_lng[1]);

        doc["id"] = std::to_string(i);
        doc["title"] = records[i][0];
        doc["loc"] = {lat, lng};
        doc["points"] = i;

        ASSERT_TRUE(coll1->add(doc.dump()).ok());
    }

    // pick a large radius covering all points, with a point close to Pantheon
    std::vector<sort_by> geo_sort_fields = {
        sort_by("loc(48.84442912268208, 2.3490714964332353)", "ASC")
    };

    auto results = coll1->search("*",
                            {}, "loc: (48.84442912268208, 2.3490714964332353, 20 km)",
                            {}, geo_sort_fields, {0}, 10, 1, FREQUENCY).get();

    ASSERT_EQ(10, results["found"].get<size_t>());

    std::vector<std::string> expected_ids = {
        "9", "7", "4", "5", "3", "8", "0", "6", "1", "2"
    };

    for(size_t i=0; i < expected_ids.size(); i++) {
        ASSERT_STREQ(expected_ids[i].c_str(), results["hits"][i]["document"]["id"].get<std::string>().c_str());
    }

    ASSERT_EQ(348, results["hits"][0]["geo_distance_meters"]["loc"].get<int>());
    ASSERT_EQ(900, results["hits"][1]["geo_distance_meters"]["loc"].get<int>());
    ASSERT_EQ(1973, results["hits"][2]["geo_distance_meters"]["loc"].get<int>());

    // desc, without filter
    geo_sort_fields = {
        sort_by("loc(48.84442912268208, 2.3490714964332353)", "DESC")
    };

    results = coll1->search("*",
                            {}, "",
                            {}, geo_sort_fields, {0}, 10, 1, FREQUENCY).get();

    ASSERT_EQ(10, results["found"].get<size_t>());

    for(size_t i=0; i < expected_ids.size(); i++) {
        ASSERT_STREQ(expected_ids[expected_ids.size() - 1 - i].c_str(), results["hits"][i]["document"]["id"].get<std::string>().c_str());
    }

    // with bad sort field formats
    std::vector<sort_by> bad_geo_sort_fields = {
        sort_by("loc(,2.3490714964332353)", "ASC")
    };

    auto res_op = coll1->search("*",
                            {}, "",
                            {}, bad_geo_sort_fields, {0}, 10, 1, FREQUENCY);

    ASSERT_FALSE(res_op.ok());
    ASSERT_STREQ("Bad syntax for geopoint sorting field `loc`", res_op.error().c_str());

    bad_geo_sort_fields = {
            sort_by("loc(x, y)", "ASC")
    };

    res_op = coll1->search("*",
                                {}, "",
                                {}, bad_geo_sort_fields, {0}, 10, 1, FREQUENCY);

    ASSERT_FALSE(res_op.ok());
    ASSERT_STREQ("Bad syntax for geopoint sorting field `loc`", res_op.error().c_str());

    bad_geo_sort_fields = {
        sort_by("loc(", "ASC")
    };

    res_op = coll1->search("*",
                           {}, "",
                           {}, bad_geo_sort_fields, {0}, 10, 1, FREQUENCY);

    ASSERT_FALSE(res_op.ok());
    ASSERT_STREQ("Could not find a field named `loc(` in the schema for sorting.", res_op.error().c_str());

    bad_geo_sort_fields = {
        sort_by("loc)", "ASC")
    };

    res_op = coll1->search("*",
                           {}, "",
                           {}, bad_geo_sort_fields, {0}, 10, 1, FREQUENCY);

    ASSERT_FALSE(res_op.ok());
    ASSERT_STREQ("Could not find a field named `loc)` in the schema for sorting.", res_op.error().c_str());

    bad_geo_sort_fields = {
            sort_by("l()", "ASC")
    };

    res_op = coll1->search("*",
                           {}, "",
                           {}, bad_geo_sort_fields, {0}, 10, 1, FREQUENCY);

    ASSERT_FALSE(res_op.ok());
    ASSERT_STREQ("Could not find a field named `l` in the schema for sorting.", res_op.error().c_str());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionSortingTest, GeoPointSortingWithExcludeRadius) {
    Collection* coll1;

    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("loc", field_types::GEOPOINT, false),
                                 field("points", field_types::INT32, false),};

    coll1 = collectionManager.get_collection("coll1").get();
    if (coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    std::vector<std::vector<std::string>> records = {
        {"Tibetan Colony",     "32.24678, 77.19239"},
        {"Civil Hospital",     "32.23959, 77.18763"},
        {"Johnson Lodge",      "32.24751, 77.18814"},

        {"Lion King Rock",     "32.24493, 77.17038"},
        {"Jai Durga Handloom", "32.25749, 77.17583"},
        {"Panduropa",          "32.26059, 77.21798"},
    };

    for (size_t i = 0; i < records.size(); i++) {
        nlohmann::json doc;

        std::vector<std::string> lat_lng;
        StringUtils::split(records[i][1], lat_lng, ", ");

        double lat = std::stod(lat_lng[0]);
        double lng = std::stod(lat_lng[1]);

        doc["id"] = std::to_string(i);
        doc["title"] = records[i][0];
        doc["loc"] = {lat, lng};
        doc["points"] = i;

        ASSERT_TRUE(coll1->add(doc.dump()).ok());
    }

    std::vector<sort_by> geo_sort_fields = {
        sort_by("loc(32.24348, 77.1893, exclude_radius: 1 km)", "ASC"),
        sort_by("points", "DESC"),
    };

    auto results = coll1->search("*",
                                 {}, "loc: (32.24348, 77.1893, 20 km)",
                                 {}, geo_sort_fields, {0}, 10, 1, FREQUENCY).get();

    ASSERT_EQ(6, results["found"].get<size_t>());

    std::vector<std::string> expected_ids = {
        "2", "1", "0", "3", "4", "5"
    };

    for (size_t i = 0; i < expected_ids.size(); i++) {
        ASSERT_STREQ(expected_ids[i].c_str(), results["hits"][i]["document"]["id"].get<std::string>().c_str());
    }

    // without exclusion filter

    geo_sort_fields = {
            sort_by("loc(32.24348, 77.1893)", "ASC"),
            sort_by("points", "DESC"),
    };

    results = coll1->search("*",
                            {}, "loc: (32.24348, 77.1893, 20 km)",
                            {}, geo_sort_fields, {0}, 10, 1, FREQUENCY).get();

    ASSERT_EQ(6, results["found"].get<size_t>());

    expected_ids = {
        "1", "2", "0", "3", "4", "5"
    };

    for (size_t i = 0; i < expected_ids.size(); i++) {
        ASSERT_STREQ(expected_ids[i].c_str(), results["hits"][i]["document"]["id"].get<std::string>().c_str());
    }

    // badly formatted exclusion filter

    geo_sort_fields = { sort_by("loc(32.24348, 77.1893, exclude_radius 1 km)", "ASC") };
    auto res_op = coll1->search("*", {}, "loc: (32.24348, 77.1893, 20 km)",
                                {}, geo_sort_fields, {0}, 10, 1, FREQUENCY);

    ASSERT_FALSE(res_op.ok());
    ASSERT_EQ("Bad syntax for geopoint sorting field `loc`", res_op.error());

    geo_sort_fields = { sort_by("loc(32.24348, 77.1893, exclude_radius: 1 meter)", "ASC") };
    res_op = coll1->search("*", {}, "loc: (32.24348, 77.1893, 20 km)",
                           {}, geo_sort_fields, {0}, 10, 1, FREQUENCY);

    ASSERT_FALSE(res_op.ok());
    ASSERT_EQ("Sort field's parameter unit must be either `km` or `mi`.", res_op.error());

    geo_sort_fields = { sort_by("loc(32.24348, 77.1893, exclude_radius: -10 km)", "ASC") };
    res_op = coll1->search("*", {}, "loc: (32.24348, 77.1893, 20 km)",
                           {}, geo_sort_fields, {0}, 10, 1, FREQUENCY);

    ASSERT_FALSE(res_op.ok());
    ASSERT_EQ("Sort field's parameter must be a positive number.", res_op.error());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionSortingTest, GeoPointSortingWithPrecision) {
    Collection* coll1;

    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("loc", field_types::GEOPOINT, false),
                                 field("points", field_types::INT32, false),};

    coll1 = collectionManager.get_collection("coll1").get();
    if (coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    std::vector<std::vector<std::string>> records = {
        {"Tibetan Colony",     "32.24678, 77.19239"},
        {"Civil Hospital",     "32.23959, 77.18763"},
        {"Johnson Lodge",      "32.24751, 77.18814"},

        {"Lion King Rock",     "32.24493, 77.17038"},
        {"Jai Durga Handloom", "32.25749, 77.17583"},
        {"Panduropa",          "32.26059, 77.21798"},

        {"Police Station",     "32.23743, 77.18639"},
        {"Panduropa Post",     "32.26263, 77.2196"},
    };

    for (size_t i = 0; i < records.size(); i++) {
        nlohmann::json doc;

        std::vector<std::string> lat_lng;
        StringUtils::split(records[i][1], lat_lng, ", ");

        double lat = std::stod(lat_lng[0]);
        double lng = std::stod(lat_lng[1]);

        doc["id"] = std::to_string(i);
        doc["title"] = records[i][0];
        doc["loc"] = {lat, lng};
        doc["points"] = i;

        ASSERT_TRUE(coll1->add(doc.dump()).ok());
    }

    std::vector<sort_by> geo_sort_fields = {
        sort_by("loc(32.24348, 77.1893, precision: 0.9 km)", "ASC"),
        sort_by("points", "DESC"),
    };

    auto results = coll1->search("*",
                                 {}, "loc: (32.24348, 77.1893, 20 km)",
                                 {}, geo_sort_fields, {0}, 10, 1, FREQUENCY).get();

    ASSERT_EQ(8, results["found"].get<size_t>());

    std::vector<std::string> expected_ids = {
        "6", "2", "1", "0", "3", "4", "7", "5"
    };

    for (size_t i = 0; i < expected_ids.size(); i++) {
        ASSERT_STREQ(expected_ids[i].c_str(), results["hits"][i]["document"]["id"].get<std::string>().c_str());
    }

    // badly formatted precision

    geo_sort_fields = { sort_by("loc(32.24348, 77.1893, precision 1 km)", "ASC") };
    auto res_op = coll1->search("*", {}, "loc: (32.24348, 77.1893, 20 km)",
                                {}, geo_sort_fields, {0}, 10, 1, FREQUENCY);

    ASSERT_FALSE(res_op.ok());
    ASSERT_EQ("Bad syntax for geopoint sorting field `loc`", res_op.error());

    geo_sort_fields = { sort_by("loc(32.24348, 77.1893, precision: 1 meter)", "ASC") };
    res_op = coll1->search("*", {}, "loc: (32.24348, 77.1893, 20 km)",
                           {}, geo_sort_fields, {0}, 10, 1, FREQUENCY);

    ASSERT_FALSE(res_op.ok());
    ASSERT_EQ("Sort field's parameter unit must be either `km` or `mi`.", res_op.error());

    geo_sort_fields = { sort_by("loc(32.24348, 77.1893, precision: -10 km)", "ASC") };
    res_op = coll1->search("*", {}, "loc: (32.24348, 77.1893, 20 km)",
                           {}, geo_sort_fields, {0}, 10, 1, FREQUENCY);

    ASSERT_FALSE(res_op.ok());
    ASSERT_EQ("Sort field's parameter must be a positive number.", res_op.error());

    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionSortingTest, GeoPointAsOptionalField) {
    Collection* coll1;

    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("loc", field_types::GEOPOINT, false, true),
                                 field("points", field_types::INT32, false),};

    coll1 = collectionManager.get_collection("coll1").get();
    if (coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    std::vector<std::vector<std::string>> records = {
        {"Tibetan Colony",     "32.24678, 77.19239"},
        {"Civil Hospital",     "32.23959, 77.18763"},
        {"Johnson Lodge",      "32.24751, 77.18814"},

        {"Lion King Rock",     "32.24493, 77.17038"},
        {"Jai Durga Handloom", "32.25749, 77.17583"},
        {"Panduropa",          "32.26059, 77.21798"},

        {"Police Station",     "32.23743, 77.18639"},
        {"Panduropa Post",     "32.26263, 77.2196"},
    };

    for (size_t i = 0; i < records.size(); i++) {
        nlohmann::json doc;

        std::vector<std::string> lat_lng;
        StringUtils::split(records[i][1], lat_lng, ", ");

        double lat = std::stod(lat_lng[0]);
        double lng = std::stod(lat_lng[1]);

        doc["id"] = std::to_string(i);
        doc["title"] = records[i][0];

        if(i != 2) {
            doc["loc"] = {lat, lng};
        }

        doc["points"] = i;

        ASSERT_TRUE(coll1->add(doc.dump()).ok());
    }

    std::vector<sort_by> geo_sort_fields = {
        sort_by("loc(32.24348, 77.1893, precision: 0.9 km)", "ASC"),
        sort_by("points", "DESC"),
    };

    auto results = coll1->search("*",
                                 {}, "loc: (32.24348, 77.1893, 20 km)",
                                 {}, geo_sort_fields, {0}, 10, 1, FREQUENCY).get();

    ASSERT_EQ(7, results["found"].get<size_t>());
    collectionManager.drop_collection("coll1");
}

TEST_F(CollectionSortingTest, GeoPointArraySorting) {
    Collection *coll1;

    std::vector<field> fields = {field("title", field_types::STRING, false),
                                 field("loc", field_types::GEOPOINT_ARRAY, false),
                                 field("points", field_types::INT32, false),};

    coll1 = collectionManager.get_collection("coll1").get();
    if(coll1 == nullptr) {
        coll1 = collectionManager.create_collection("coll1", 1, fields, "points").get();
    }

    std::vector<std::vector<std::vector<std::string>>> records = {
        {   {"Alpha Inc", "Ennore", "13.22112, 80.30511"},
            {"Alpha Inc", "Velachery", "12.98973, 80.23095"}
        },

        {
            {"Veera Inc", "Thiruvallur", "13.12752, 79.90136"},
        },

        {
            {"B1 Inc", "Bengaluru", "12.98246, 77.5847"},
            {"B1 Inc", "Hosur", "12.74147, 77.82915"},
            {"B1 Inc", "Vellore", "12.91866, 79.13075"},
        },

        {
            {"M Inc", "Nashik", "20.11282, 73.79458"},
            {"M Inc", "Pune", "18.56309, 73.855"},
        }
    };

    for(size_t i=0; i<records.size(); i++) {
        nlohmann::json doc;

        doc["id"] = std::to_string(i);
        doc["title"] = records[i][0][0];
        doc["points"] = i;

        std::vector<std::vector<double>> lat_lngs;
        for(size_t k = 0; k < records[i].size(); k++) {
            std::vector<std::string> lat_lng_str;
            StringUtils::split(records[i][k][2], lat_lng_str, ", ");

            std::vector<double> lat_lng = {
                    std::stod(lat_lng_str[0]),
                    std::stod(lat_lng_str[1])
            };

            lat_lngs.push_back(lat_lng);
        }

        doc["loc"] = lat_lngs;
        auto add_op = coll1->add(doc.dump());
        ASSERT_TRUE(add_op.ok());
    }

    std::vector<sort_by> geo_sort_fields = {
        sort_by("loc(13.12631, 80.20252)", "ASC"),
        sort_by("points", "DESC"),
    };

    // pick a location close to Chennai
    auto results = coll1->search("*",
                                 {}, "loc: (13.12631, 80.20252, 100 km)",
                                 {}, geo_sort_fields, {0}, 10, 1, FREQUENCY).get();

    ASSERT_EQ(2, results["found"].get<size_t>());
    ASSERT_EQ(2, results["hits"].size());

    ASSERT_STREQ("0", results["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("1", results["hits"][1]["document"]["id"].get<std::string>().c_str());

    // pick a large radius covering all points

    geo_sort_fields = {
        sort_by("loc(13.03388, 79.25868)", "ASC"),
        sort_by("points", "DESC"),
    };

    results = coll1->search("*",
                            {}, "loc: (13.03388, 79.25868, 1000 km)",
                            {}, geo_sort_fields, {0}, 10, 1, FREQUENCY).get();

    ASSERT_EQ(4, results["found"].get<size_t>());

    ASSERT_STREQ("2", results["hits"][0]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("1", results["hits"][1]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("0", results["hits"][2]["document"]["id"].get<std::string>().c_str());
    ASSERT_STREQ("3", results["hits"][3]["document"]["id"].get<std::string>().c_str());

    collectionManager.drop_collection("coll1");
}
