#include <gtest/gtest.h>

#include <formats/json/parser/parser.hpp>
#include <formats/json/serialize.hpp>

// TODO: move to utest/*
#define EXPECT_THROW_TEXT(code, exception_type, exc_text)                    \
  try {                                                                      \
    code;                                                                    \
    FAIL() << "expected exception " #exception_type ", but not thrown";      \
  } catch (const exception_type& e) {                                        \
    EXPECT_EQ(e.what(), std::string{exc_text}) << "wrong exception message"; \
  } catch (const std::exception& e) {                                        \
    FAIL() << "wrong exception type, expected " #exception_type ", but got " \
           << typeid(e).name();                                              \
  }

using namespace formats::json::parser;

TEST(JsonStringParser, Int64) {
  std::string input{"12345"};

  int64_t result{0};
  Int64Parser int_parser;
  int_parser.Reset(result);

  ParserState state;
  state.PushParserNoKey(int_parser);
  state.ProcessInput(input);

  EXPECT_EQ(result, 12345);
}

TEST(JsonStringParser, Int64Overflow) {
  std::string input{std::to_string(-1ULL)};

  EXPECT_THROW_TEXT((ParseToType<int64_t, Int64Parser>(input)), ParseError,
                    "Parse error at pos 20, path '': bad "
                    "numeric conversion: positive overflow");
}

class EmptyObjectParser final : public BaseParser {
 protected:
  void StartObject() override {}

  void EndObject() override { parser_state_->PopMe(*this); }

  std::string Expected() const override { return "'}'"; }
};

TEST(JsonStringParser, EmptyObject) {
  std::string input{"{}"};

  EmptyObjectParser obj_parser;

  ParserState state;
  state.PushParserNoKey(obj_parser);
  EXPECT_NO_THROW(state.ProcessInput(input));
}

TEST(JsonStringParser, EmptyObjectKey) {
  std::string input{"{\"key\":1}"};

  EmptyObjectParser obj_parser;

  ParserState state;
  state.PushParserNoKey(obj_parser);
  EXPECT_THROW_TEXT(state.ProcessInput(input), ParseError,
                    "Parse error at pos 6, path '': '}' was "
                    "expected, but field 'key' found");
}

struct IntObject final {
  int64_t field;
};

bool operator==(const IntObject& lh, const IntObject& rh) {
  return lh.field == rh.field;
}

class IntObjectParser final : public BaseParser {
 public:
  void Reset(IntObject& obj) {
    result_ = &obj;
    has_field_ = false;
  }

 protected:
  void StartObject() override {}

  void Key(std::string_view key) override {
    if (key == "field") {
      has_field_ = true;
      field_parser_.Reset(result_->field);
      parser_state_->PushParser(field_parser_, key);
    } else {
      throw InternalParseError("Bad field for IntObject ('" + std::string(key) +
                               ")");
    }
  }

  void EndObject() override {
    if (!has_field_) {
      throw InternalParseError("Missing required field 'field'");
    }
    parser_state_->PopMe(*this);
  }

  // Note: not strictly correct
  std::string Expected() const override { return "'{'"; }

 private:
  Int64Parser field_parser_;
  IntObject* result_{nullptr};
  bool has_field_;
};

TEST(JsonStringParser, IntObject) {
  std::string input("{\"field\": 234}");
  EXPECT_EQ((ParseToType<IntObject, IntObjectParser>(input)), IntObject({234}));
}

TEST(JsonStringParser, IntObjectNoField) {
  std::string input("{}");

  EXPECT_THROW_TEXT(
      (ParseToType<IntObject, IntObjectParser>(input)), ParseError,
      "Parse error at pos 1, path '': Missing required field 'field'");
}

TEST(JsonStringParser, ArrayIntObjectNoField) {
  std::string input("[{}]");

  IntObjectParser obj_parser;
  ArrayParser<IntObject, IntObjectParser> array_parser(obj_parser);

  std::vector<IntObject> result;
  array_parser.Reset(result);
  ParserState state;
  state.PushParserNoKey(array_parser);

  EXPECT_THROW_TEXT(
      state.ProcessInput(input), ParseError,
      "Parse error at pos 2, path '[0]': Missing required field 'field'");
}

TEST(JsonStringParser, ArrayInt) {
  std::string input("[1,2,3]");
  std::vector<int64_t> result{};

  Int64Parser int_parser;
  ArrayParser<int64_t, Int64Parser> parser(int_parser);
  parser.Reset(result);

  ParserState state;
  state.PushParserNoKey(parser);
  state.ProcessInput(input);
  EXPECT_EQ(result, (std::vector<int64_t>{1, 2, 3}));
}

TEST(JsonStringParser, ArrayArrayInt) {
  std::string input("[[1],[],[2,3,4]]");
  std::vector<std::vector<int64_t>> result{};

  Int64Parser int_parser;
  using Subparser = ArrayParser<int64_t, Int64Parser>;
  Subparser subparser(int_parser);
  ArrayParser<std::vector<int64_t>, Subparser> parser(subparser);
  parser.Reset(result);

  ParserState state;
  state.PushParserNoKey(parser);
  state.ProcessInput(input);
  EXPECT_EQ(result, (std::vector<std::vector<int64_t>>{{1}, {}, {2, 3, 4}}));
}

template <class T>
struct JsonStringParserMap : public ::testing::Test {
  using Map = T;
};

using Maps = ::testing::Types<std::map<std::string, int>,
                              std::unordered_map<std::string, int>>;
TYPED_TEST_SUITE(JsonStringParserMap, Maps);

TYPED_TEST(JsonStringParserMap, Map) {
  using Map = typename TestFixture::Map;

  IntParser int_parser;
  MapParser<Map, IntParser> parser{int_parser};

  Map result;
  parser.Reset(result);
  ParserState state;
  state.PushParserNoKey(parser);
  state.ProcessInput(R"({"key": 1, "other": 3})");
  EXPECT_EQ(result, (Map{{"key", 1}, {"other", 3}}));
}

TYPED_TEST(JsonStringParserMap, Empty) {
  using Map = typename TestFixture::Map;

  IntParser int_parser;
  MapParser<Map, IntParser> parser{int_parser};

  Map result;
  parser.Reset(result);
  ParserState state;
  state.PushParserNoKey(parser);
  state.ProcessInput(R"({})");
  EXPECT_EQ(result, (Map{}));
}

TYPED_TEST(JsonStringParserMap, Invalid) {
  using Map = typename TestFixture::Map;

  IntParser int_parser;
  MapParser<Map, IntParser> parser{int_parser};

  Map result;
  parser.Reset(result);
  ParserState state;
  state.PushParserNoKey(parser);

  EXPECT_THROW_TEXT(
      state.ProcessInput(R"(123)"), ParseError,
      "Parse error at pos 3, path '': object was expected, but integer found");

  EXPECT_THROW_TEXT(
      state.ProcessInput(R"({{"key": 1}})"), ParseError,
      "Parse error at pos 1, path '': Missing a name for object member.");

  EXPECT_THROW_TEXT(state.ProcessInput(R"(}{)"), ParseError,
                    "Parse error at pos 0, path '': The document is empty.");
}

TEST(JsonStringParser, JsonValue) {
  std::string input{};

  std::string inputs[] = {
      R"([1, "123", "", -2, 3.5, {"key": 1, "other": {"key2": 2}}, {}])",
      R"({})",
  };
  for (const auto& input : inputs) {
    auto value_str = formats::json::FromString(input);
    auto value_sax = ParseToType<formats::json::Value, JsonValueParser>(input);
    EXPECT_EQ(value_str, value_sax) << "input: " + input + ", str='" +
                                           ToString(value_str) + "', sax='" +
                                           ToString(value_sax) + "'";
  }
}

TEST(JsonStringParser, JsonValueBad) {
  std::string input{};

  std::string inputs[] = {
      R"({)",         //
      R"()",          //
      R"({}})",       //
      R"(})",         //
      R"({"key")",    //
      R"({"key)",     //
      R"({"key":1)",  //
      R"([)",         //
      R"(1 2)",       //
  };
  for (const auto& input : inputs) {
    EXPECT_THROW((ParseToType<formats::json::Value, JsonValueParser>(input)),
                 ParseError);
  }
}