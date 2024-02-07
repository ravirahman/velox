/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/functions/prestosql/tests/CastBaseTest.h"
#include "velox/functions/prestosql/types/JsonType.h"

using namespace facebook::velox;

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

template <typename T>
using TwoDimVector = std::vector<std::vector<std::optional<T>>>;

template <typename TKey, typename TValue>
using Pair = std::pair<TKey, std::optional<TValue>>;

} // namespace

using JsonNativeType = StringView;

class JsonCastTest : public functions::test::CastBaseTest {
 protected:
  template <typename TFrom>
  void testCastToJson(
      const TypePtr& fromType,
      std::vector<std::optional<TFrom>> input,
      std::vector<std::optional<JsonNativeType>> expected) {
    return testCast<TFrom, JsonNativeType>(fromType, JSON(), input, expected);
  }

  template <typename T>
  void testCastFromArray(
      const TypePtr& fromType,
      const TwoDimVector<T>& input,
      const std::vector<std::optional<JsonNativeType>>& expected) {
    auto arrayVector = makeNullableArrayVector<T>(input, fromType);
    auto expectedVector =
        makeNullableFlatVector<JsonNativeType>(expected, JSON());

    testCast(arrayVector, expectedVector);
  }

  template <typename TKey, typename TValue>
  void testCastFromMap(
      const TypePtr& fromType,
      const std::vector<std::vector<Pair<TKey, TValue>>>& input,
      const std::vector<std::optional<JsonNativeType>>& expected) {
    auto mapVector = makeMapVector<TKey, TValue>(input, fromType);
    auto expectedVector =
        makeNullableFlatVector<JsonNativeType>(expected, JSON());

    testCast(mapVector, expectedVector);
  }

  template <typename TChild1, typename TChild2, typename TChild3>
  void testCastFromRow(
      const TypePtr& fromType,
      const std::vector<std::optional<TChild1>>& child1,
      const std::vector<std::optional<TChild2>>& child2,
      const std::vector<std::optional<TChild3>>& child3,
      const std::vector<std::optional<JsonNativeType>>& expected) {
    auto firstChild =
        makeNullableFlatVector<TChild1>(child1, fromType->childAt(0));
    auto secondChild =
        makeNullableFlatVector<TChild2>(child2, fromType->childAt(1));
    auto thirdChild =
        makeNullableFlatVector<TChild3>(child3, fromType->childAt(2));

    auto rowVector = makeRowVector({firstChild, secondChild, thirdChild});
    auto expectedVector =
        makeNullableFlatVector<JsonNativeType>(expected, JSON());

    testCast(rowVector, expectedVector);
  }

  // Populates offsets and sizes buffers for making array and map vectors.
  // Every row has offsetEvery number of elements except the last row.
  void makeOffsetsAndSizes(
      int numOfElements,
      int offsetEvery,
      BufferPtr& offsets,
      BufferPtr& sizes) {
    auto rawOffsets = offsets->asMutable<vector_size_t>();
    auto rawSizes = sizes->asMutable<vector_size_t>();

    for (auto i = 0; i < numOfElements; i += offsetEvery) {
      rawOffsets[i / offsetEvery] = i;
      rawSizes[i / offsetEvery] =
          i + offsetEvery > numOfElements ? numOfElements - i : offsetEvery;
    }
  }

  // Makes a flat vector wrapped in reversed indices. If isKey is false, also
  // makes the first row to be null.
  template <typename T>
  VectorPtr makeDictionaryVector(
      const std::vector<std::optional<T>>& data,
      const TypePtr& type = CppToType<T>::create(),
      bool isKey = false) {
    VectorPtr vector;
    if constexpr (std::is_same_v<T, UnknownValue>) {
      vector = makeFlatUnknownVector(data.size());
    } else {
      vector = makeNullableFlatVector<T>(data, type);
    }

    auto reversedIndices = makeIndicesInReverse(data.size());

    if (!isKey) {
      auto nulls = makeNulls(data.size(), [](auto row) { return row == 0; });
      return BaseVector::wrapInDictionary(
          nulls, reversedIndices, data.size(), vector);
    } else {
      return BaseVector::wrapInDictionary(
          nullptr, reversedIndices, data.size(), vector);
    }
  }

  // Makes an array vector whose elements vector is wrapped in a dictionary
  // that reverses all elements and first element is null. Each row of the array
  // vector contains arraySize number of elements except the last row.
  template <typename T>
  ArrayVectorPtr makeArrayWithDictionaryElements(
      const std::vector<std::optional<T>>& elements,
      int arraySize,
      const TypePtr& type = ARRAY(CppToType<T>::create())) {
    int size = elements.size();
    int numOfArray = (size + arraySize - 1) / arraySize;
    auto dictElements = makeDictionaryVector(elements, type->childAt(0));

    BufferPtr offsets = allocateOffsets(numOfArray, pool());
    BufferPtr sizes = allocateSizes(numOfArray, pool());
    makeOffsetsAndSizes(size, arraySize, offsets, sizes);

    return std::make_shared<ArrayVector>(
        pool(), type, nullptr, numOfArray, offsets, sizes, dictElements);
  }

  // Makes a map vector whose keys and values vectors are wrapped in a
  // dictionary that reverses all elements and first value is null. Each row of
  // the map vector contains mapSize number of keys and values except the last
  // row.
  template <typename TKey, typename TValue>
  MapVectorPtr makeMapWithDictionaryElements(
      const std::vector<std::optional<TKey>>& keys,
      const std::vector<std::optional<TValue>>& values,
      int mapSize,
      const TypePtr& type =
          MAP(CppToType<TKey>::create(), CppToType<TValue>::create())) {
    VELOX_CHECK_EQ(
        keys.size(),
        values.size(),
        "keys and values must have the same number of elements.");

    int size = keys.size();
    int numOfMap = (size + mapSize - 1) / mapSize;
    auto dictKeys = makeDictionaryVector(keys, type->childAt(0), true);
    auto dictValues = makeDictionaryVector(values, type->childAt(1));

    BufferPtr offsets = allocateOffsets(numOfMap, pool());
    BufferPtr sizes = allocateSizes(numOfMap, pool());
    makeOffsetsAndSizes(size, mapSize, offsets, sizes);

    return std::make_shared<MapVector>(
        pool(), type, nullptr, numOfMap, offsets, sizes, dictKeys, dictValues);
  }

  // Makes a row vector whose children vectors are wrapped in a dictionary
  // that reverses all elements and elements at the first row are null.
  template <typename T>
  RowVectorPtr makeRowWithDictionaryElements(
      const TwoDimVector<T>& elements,
      const TypePtr& rowType) {
    VELOX_CHECK_NE(elements.size(), 0, "At least one child must be provided.");

    int childrenSize = elements.size();
    int size = elements[0].size();

    std::vector<VectorPtr> dictChildren;
    for (int i = 0; i < childrenSize; ++i) {
      VELOX_CHECK_EQ(
          elements[i].size(),
          size,
          "All children vectors must have the same size.");
      dictChildren.push_back(
          makeDictionaryVector(elements[i], rowType->childAt(i)));
    }

    return std::make_shared<RowVector>(
        pool(), rowType, nullptr, size, dictChildren);
  }

  VectorPtr makeFlatUnknownVector(int size) {
    auto vector =
        BaseVector::create<FlatVector<UnknownValue>>(UNKNOWN(), size, pool());
    for (int i = 0; i < size; ++i) {
      vector->setNull(i, true);
    }

    return vector;
  }
};

TEST_F(JsonCastTest, fromInteger) {
  testCastToJson<int64_t>(
      BIGINT(),
      {1, -3, 0, INT64_MAX, INT64_MIN, std::nullopt},
      {"1"_sv,
       "-3"_sv,
       "0"_sv,
       "9223372036854775807"_sv,
       "-9223372036854775808"_sv,
       std::nullopt});
  testCastToJson<int8_t>(
      TINYINT(),
      {1, -3, 0, INT8_MAX, INT8_MIN, std::nullopt},
      {"1"_sv, "-3"_sv, "0"_sv, "127"_sv, "-128"_sv, std::nullopt});
  testCastToJson<int32_t>(
      INTEGER(),
      {std::nullopt, std::nullopt, std::nullopt, std::nullopt},
      {std::nullopt, std::nullopt, std::nullopt, std::nullopt});
}

TEST_F(JsonCastTest, fromVarchar) {
  testCastToJson<StringView>(VARCHAR(), {"\U0001F64F"}, {"\"\\ud83d\\ude4f\""});
  testCastToJson<StringView>(
      VARCHAR(),
      {"aaa"_sv, "bbb"_sv, "ccc"_sv},
      {R"("aaa")"_sv, R"("bbb")"_sv, R"("ccc")"_sv});
  testCastToJson<StringView>(
      VARCHAR(),
      {""_sv,
       std::nullopt,
       "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f\"\\ ."_sv},
      {"\"\""_sv,
       std::nullopt,
       R"("\u0001\u0002\u0003\u0004\u0005\u0006\u0007\b\t\n\u000b\f\r\u000e\u000f\u0010\u0011\u0012\u0013\u0014\u0015\u0016\u0017\u0018\u0019\u001a\u001b\u001c\u001d\u001e\u001f\"\\ .")"_sv});
  testCastToJson<StringView>(
      VARCHAR(),
      {std::nullopt, std::nullopt, std::nullopt, std::nullopt},
      {std::nullopt, std::nullopt, std::nullopt, std::nullopt});
}

TEST_F(JsonCastTest, fromBoolean) {
  testCastToJson<bool>(
      BOOLEAN(),
      {true, false, false, std::nullopt},
      {"true"_sv, "false"_sv, "false"_sv, std::nullopt});
  testCastToJson<bool>(
      BOOLEAN(),
      {std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
      {std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt});
}

TEST_F(JsonCastTest, fromDoubleAndReal) {
  testCastToJson<double>(
      DOUBLE(),
      {1.1,
       2.0001,
       10.0,
       3.14e0,
       -0.0,
       0.00012,
       -0.001,
       12345.0,
       10'000'000.0,
       123456789.01234567,
       kNan,
       -kNan,
       kInf,
       -kInf,
       std::nullopt},
      {"1.1"_sv,
       "2.0001"_sv,
       "10.0"_sv,
       "3.14"_sv,
       "-0.0"_sv,
       "1.2E-4"_sv,
       "-0.001"_sv,
       "12345.0"_sv,
       "1.0E7"_sv,
       "1.2345678901234567E8"_sv,
       "NaN"_sv,
       "NaN"_sv,
       "Infinity"_sv,
       "-Infinity"_sv,
       std::nullopt});
  testCastToJson<float>(
      REAL(),
      {1.1,
       2.0001,
       10.0,
       3.14e0,
       -0.0,
       0.00012,
       -0.001,
       12345.0,
       10'000'000.0,
       123456780.0,
       kNan,
       -kNan,
       kInf,
       -kInf,
       std::nullopt},
      {"1.1"_sv,
       "2.0001"_sv,
       "10.0"_sv,
       "3.14"_sv,
       "-0.0"_sv,
       "1.2E-4"_sv,
       "-0.001"_sv,
       "12345.0"_sv,
       "1.0E7"_sv,
       "1.2345678E8"_sv,
       "NaN"_sv,
       "NaN"_sv,
       "Infinity"_sv,
       "-Infinity"_sv,
       std::nullopt});

  testCastToJson<double>(
      DOUBLE(),
      {std::nullopt, std::nullopt, std::nullopt, std::nullopt},
      {std::nullopt, std::nullopt, std::nullopt, std::nullopt});
}

TEST_F(JsonCastTest, fromDate) {
  testCastToJson<int32_t>(
      DATE(),
      {0, 1000, -10000, std::nullopt},
      {"1970-01-01"_sv, "1972-09-27"_sv, "1942-08-16"_sv, std::nullopt});
  testCastToJson<int32_t>(
      DATE(),
      {std::nullopt, std::nullopt, std::nullopt, std::nullopt},
      {std::nullopt, std::nullopt, std::nullopt, std::nullopt});
}

TEST_F(JsonCastTest, fromTimestamp) {
  testCastToJson<Timestamp>(
      TIMESTAMP(),
      {Timestamp{0, 0},
       Timestamp{10000000, 0},
       Timestamp{-1, 9000},
       std::nullopt},
      {"1970-01-01T00:00:00.000000000"_sv,
       "1970-04-26T17:46:40.000000000"_sv,
       "1969-12-31T23:59:59.000009000"_sv,
       std::nullopt});
  testCastToJson<Timestamp>(
      TIMESTAMP(),
      {std::nullopt, std::nullopt, std::nullopt, std::nullopt},
      {std::nullopt, std::nullopt, std::nullopt, std::nullopt});
}

TEST_F(JsonCastTest, fromUnknown) {
  auto input = makeFlatUnknownVector(3);
  auto expected = makeNullableFlatVector<JsonNativeType>(
      {std::nullopt, std::nullopt, std::nullopt}, JSON());
  evaluateAndVerify(UNKNOWN(), JSON(), makeRowVector({input}), expected);
}

TEST_F(JsonCastTest, toArrayOfJson) {
  auto arrays = makeArrayVectorFromJson<int64_t>({
      "[1, 2, 3]",
      "[4, 5]",
      "[6, 7, 8]",
  });

  auto from = makeArrayVector({0, 2}, arrays);

  auto to = makeArrayVector<JsonNativeType>(
      {
          {"[1,2,3]", "[4,5]"},
          {"[6,7,8]"},
      },
      JSON());

  testCast(from, to);
  testCast(to, from);
}

TEST_F(JsonCastTest, fromArray) {
  TwoDimVector<StringView> array{
      {"red"_sv, "blue"_sv}, {std::nullopt, std::nullopt, "purple"_sv}, {}};
  std::vector<std::optional<JsonNativeType>> expected{
      R"(["red","blue"])", R"([null,null,"purple"])", "[]"};

  // Tests array of json elements.
  std::vector<std::optional<JsonNativeType>> expectedJsonArray{
      "[red,blue]", "[null,null,purple]", "[]"};
  testCastFromArray(ARRAY(JSON()), array, expectedJsonArray);

  // Tests array whose elements are of unknown type.
  auto arrayOfUnknownElements = makeArrayWithDictionaryElements<UnknownValue>(
      {std::nullopt, std::nullopt, std::nullopt, std::nullopt},
      2,
      ARRAY(UNKNOWN()));
  auto arrayOfUnknownElementsExpected = makeNullableFlatVector<JsonNativeType>(
      {"[null,null]", "[null,null]"}, JSON());
  testCast(arrayOfUnknownElements, arrayOfUnknownElementsExpected);

  // Tests array whose elements are wrapped in a dictionary.
  auto arrayOfDictElements =
      makeArrayWithDictionaryElements<int64_t>({1, -2, 3, -4, 5, -6, 7}, 2);
  auto arrayOfDictElementsExpected = makeNullableFlatVector<JsonNativeType>(
      {"[null,-6]", "[5,-4]", "[3,-2]", "[1]"}, JSON());
  testCast(arrayOfDictElements, arrayOfDictElementsExpected);

  // Tests array whose elements are json and wrapped in a dictionary.
  auto jsonArrayOfDictElements =
      makeArrayWithDictionaryElements<JsonNativeType>(
          {"a"_sv, "b"_sv, "c"_sv, "d"_sv, "e"_sv, "f"_sv, "g"_sv},
          2,
          ARRAY(JSON()));
  auto jsonArrayOfDictElementsExpected = makeNullableFlatVector<JsonNativeType>(
      {"[null,f]", "[e,d]", "[c,b]", "[a]"}, JSON());
  testCast(jsonArrayOfDictElements, jsonArrayOfDictElementsExpected);

  // Tests array vector with nulls at all rows.
  auto allNullArray = makeAllNullArrayVector(5, BIGINT());
  auto allNullExpected = makeNullableFlatVector<JsonNativeType>(
      {std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
      JSON());
  testCast(allNullArray, allNullExpected);
}

TEST_F(JsonCastTest, fromAllNullOrEmptyArrayOfRows) {
  // ARRAY(CONSTANT(ROW)) with all null or empty elements.
  auto elements =
      BaseVector::createNullConstant(ROW({"c0"}, {VARCHAR()}), 0, pool());
  auto data = makeArrayVector({0, 0, 0, 0}, elements, {0, 2});

  auto expected = makeNullableFlatVector<JsonNativeType>(
      {std::nullopt, "[]", std::nullopt, "[]"}, JSON());
  testCast(data, expected);
}

TEST_F(JsonCastTest, fromAllNullOrEmptyMapOfRows) {
  // MAP(..., CONSTANT(ROW)) with all null or empty elements.
  auto keys = makeNullConstant(TypeKind::INTEGER, 0);
  auto values =
      BaseVector::createNullConstant(ROW({"c0"}, {VARCHAR()}), 0, pool());
  auto data = makeMapVector({0, 0, 0, 0}, keys, values, {0, 2});

  auto expected = makeNullableFlatVector<JsonNativeType>(
      {std::nullopt, "{}", std::nullopt, "{}"}, JSON());
  testCast(data, expected);
}

TEST_F(JsonCastTest, fromMap) {
  // Tests map with string keys.
  std::vector<std::vector<Pair<StringView, int64_t>>> mapStringKey{
      {{"blue"_sv, 1}, {"red"_sv, 2}},
      {{"purple", std::nullopt}, {"orange"_sv, -2}},
      {}};
  std::vector<std::optional<JsonNativeType>> expectedStringKey{
      R"({"blue":1,"red":2})", R"({"orange":-2,"purple":null})", "{}"};
  testCastFromMap(MAP(VARCHAR(), BIGINT()), mapStringKey, expectedStringKey);

  // Tests map with integer keys.
  std::vector<std::vector<Pair<int16_t, int64_t>>> mapIntKey{
      {{3, std::nullopt}, {4, 2}}, {}};
  std::vector<std::optional<JsonNativeType>> expectedIntKey{
      R"({"3":null,"4":2})", "{}"};
  testCastFromMap(MAP(SMALLINT(), BIGINT()), mapIntKey, expectedIntKey);

  // Tests map with floating-point keys.
  std::vector<std::vector<Pair<double, int64_t>>> mapDoubleKey{
      {{4.4, std::nullopt}, {3.3, 2}, {10.0, 9}, {-100000000.5, 99}}, {}};
  std::vector<std::optional<JsonNativeType>> expectedDoubleKey{
      R"({"-1.000000005E8":99,"10.0":9,"3.3":2,"4.4":null})", "{}"};
  testCastFromMap(MAP(DOUBLE(), BIGINT()), mapDoubleKey, expectedDoubleKey);

  // Tests map with boolean keys.
  std::vector<std::vector<Pair<bool, int64_t>>> mapBoolKey{
      {{true, std::nullopt}, {false, 2}}, {}};
  std::vector<std::optional<JsonNativeType>> expectedBoolKey{
      R"({"false":2,"true":null})", "{}"};
  testCastFromMap(MAP(BOOLEAN(), BIGINT()), mapBoolKey, expectedBoolKey);

  // Tests map whose values are of unknown type.
  std::vector<std::optional<StringView>> keys{
      "a"_sv, "b"_sv, "c"_sv, "d"_sv, "e"_sv, "f"_sv, "g"_sv};
  std::vector<std::optional<UnknownValue>> unknownValues{
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt};
  auto mapOfUnknownValues =
      makeMapWithDictionaryElements<StringView, UnknownValue>(
          keys, unknownValues, 2, MAP(VARCHAR(), UNKNOWN()));

  auto mapOfUnknownValuesExpected = makeNullableFlatVector<JsonNativeType>(
      {R"({"f":null,"g":null})",
       R"({"d":null,"e":null})",
       R"({"b":null,"c":null})",
       R"({"a":null})"},
      JSON());

  testCast(mapOfUnknownValues, mapOfUnknownValuesExpected);

  // Tests map whose elements are wrapped in a dictionary.
  std::vector<std::optional<double>> values{
      1.1e3, 2.2, 3.14e0, -4.4, std::nullopt, -0.0000000006, -7.7};
  auto mapOfDictElements = makeMapWithDictionaryElements(keys, values, 2);

  auto mapOfDictElementsExpected = makeNullableFlatVector<JsonNativeType>(
      {R"({"f":-6.0E-10,"g":null})",
       R"({"d":-4.4,"e":null})",
       R"({"b":2.2,"c":3.14})",
       R"({"a":1100.0})"},
      JSON());
  testCast(mapOfDictElements, mapOfDictElementsExpected);

  // Tests map whose elements are json and wrapped in a dictionary.
  auto jsonMapOfDictElements =
      makeMapWithDictionaryElements(keys, values, 2, MAP(JSON(), DOUBLE()));
  auto jsonMapOfDictElementsExpected = makeNullableFlatVector<JsonNativeType>(
      {"{f:-6.0E-10,g:null}",
       "{d:-4.4,e:null}",
       "{b:2.2,c:3.14}",
       "{a:1100.0}"},
      JSON());
  testCast(jsonMapOfDictElements, jsonMapOfDictElementsExpected);

  // Tests map vector with nulls at all rows.
  auto allNullMap = makeAllNullMapVector(5, VARCHAR(), BIGINT());
  auto allNullExpected = makeNullableFlatVector<JsonNativeType>(
      {std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
      JSON());
  testCast(allNullMap, allNullExpected);
}

TEST_F(JsonCastTest, fromRow) {
  std::vector<std::optional<int64_t>> child1{
      std::nullopt, 2, 3, std::nullopt, 5};
  std::vector<std::optional<StringView>> child2{
      "red"_sv, std::nullopt, "blue"_sv, std::nullopt, "yellow"_sv};
  std::vector<std::optional<double>> child3{
      1.1, 2.2, std::nullopt, std::nullopt, 5.5};
  std::vector<std::optional<JsonNativeType>> expected{
      R"([null,"red",1.1])",
      R"([2,null,2.2])",
      R"([3,"blue",null])",
      R"([null,null,null])",
      R"([5,"yellow",5.5])"};
  testCastFromRow<int64_t, StringView, double>(
      ROW({BIGINT(), VARCHAR(), DOUBLE()}), child1, child2, child3, expected);

  // Tests row with json child column.
  std::vector<std::optional<JsonNativeType>> expectedJsonChild{
      R"([null,red,1.1])",
      R"([2,null,2.2])",
      R"([3,blue,null])",
      R"([null,null,null])",
      R"([5,yellow,5.5])"};
  testCastFromRow<int64_t, StringView, double>(
      ROW({BIGINT(), JSON(), DOUBLE()}),
      child1,
      child2,
      child3,
      expectedJsonChild);

  // Tests row whose children are of unknown type.
  auto rowOfUnknownChildren = makeRowWithDictionaryElements<UnknownValue>(
      {{std::nullopt, std::nullopt}, {std::nullopt, std::nullopt}},
      ROW({UNKNOWN(), UNKNOWN()}));
  auto rowOfUnknownChildrenExpected = makeNullableFlatVector<JsonNativeType>(
      {"[null,null]", "[null,null]"}, JSON());

  testCast(rowOfUnknownChildren, rowOfUnknownChildrenExpected);

  // Tests row whose children are wrapped in dictionaries.
  auto rowOfDictElements = makeRowWithDictionaryElements<int64_t>(
      {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}}, ROW({BIGINT(), BIGINT(), BIGINT()}));
  auto rowOfDictElementsExpected = makeNullableFlatVector<JsonNativeType>(
      {"[null,null,null]", "[2,5,8]", "[1,4,7]"}, JSON());
  testCast(rowOfDictElements, rowOfDictElementsExpected);

  // Tests row whose children are json and wrapped in dictionaries.
  auto jsonRowOfDictElements = makeRowWithDictionaryElements<JsonNativeType>(
      {{"a1"_sv, "a2"_sv, "a3"_sv},
       {"b1"_sv, "b2"_sv, "b3"_sv},
       {"c1"_sv, "c2"_sv, "c3"_sv}},
      ROW({JSON(), JSON(), JSON()}));
  auto jsonRowOfDictElementsExpected = makeNullableFlatVector<JsonNativeType>(
      {"[null,null,null]", "[a2,b2,c2]", "[a1,b1,c1]"}, JSON());
  testCast(jsonRowOfDictElements, jsonRowOfDictElementsExpected);

  // Tests row vector with nulls at all rows.
  auto allNullChild = makeAllNullFlatVector<int64_t>(5);
  auto nulls = makeNulls(5, [](auto /*row*/) { return true; });

  auto allNullRow = std::make_shared<RowVector>(
      pool(), ROW({BIGINT()}), nulls, 5, std::vector<VectorPtr>{allNullChild});
  auto allNullExpected = makeNullableFlatVector<JsonNativeType>(
      {std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt},
      JSON());
  testCast(allNullRow, allNullExpected);
}

TEST_F(JsonCastTest, fromNested) {
  // Create map of array vector.
  auto keyVector = makeNullableFlatVector<StringView>(
      {"blue"_sv, "red"_sv, "green"_sv, "yellow"_sv, "purple"_sv, "orange"_sv},
      JSON());
  auto valueVector = makeNullableArrayVector<int64_t>(
      {{1, 2},
       {std::nullopt, 4},
       {std::nullopt, std::nullopt},
       {7, 8},
       {9, std::nullopt},
       {11, 12}});

  auto offsets = allocateOffsets(3, pool());
  auto sizes = allocateSizes(3, pool());
  makeOffsetsAndSizes(6, 2, offsets, sizes);

  auto nulls = makeNulls({false, true, false});

  auto mapVector = std::make_shared<MapVector>(
      pool(),
      MAP(JSON(), ARRAY(BIGINT())),
      nulls,
      3,
      offsets,
      sizes,
      keyVector,
      valueVector);

  // Create array of map vector
  std::vector<Pair<StringView, int64_t>> a{{"blue"_sv, 1}, {"red"_sv, 2}};
  std::vector<Pair<StringView, int64_t>> b{{"green"_sv, std::nullopt}};
  std::vector<Pair<StringView, int64_t>> c{{"yellow"_sv, 4}, {"purple"_sv, 5}};
  std::vector<std::vector<std::vector<Pair<StringView, int64_t>>>> data{
      {a, b}, {b}, {c, a}};

  auto arrayVector = makeArrayOfMapVector<StringView, int64_t>(data);

  // Create row vector of array of map and map of array
  auto rowVector = makeRowVector({mapVector, arrayVector});

  std::vector<std::optional<JsonNativeType>> expected{
      R"([{blue:[1,2],red:[null,4]},[{"blue":1,"red":2},{"green":null}]])",
      R"([null,[{"green":null}]])",
      R"([{orange:[11,12],purple:[9,null]},[{"purple":5,"yellow":4},{"blue":1,"red":2}]])"};
  auto expectedVector =
      makeNullableFlatVector<JsonNativeType>(expected, JSON());

  testCast(rowVector, expectedVector);
}

TEST_F(JsonCastTest, unsupportedTypes) {
  // Map keys cannot be timestamp.
  auto timestampKeyMap = makeMapVector<Timestamp, int64_t>({{}});
  VELOX_ASSERT_THROW(
      evaluateCast(
          MAP(TIMESTAMP(), BIGINT()), JSON(), makeRowVector({timestampKeyMap})),
      "Cannot cast MAP<TIMESTAMP,BIGINT> to JSON");

  // All children of row must be of supported types.
  auto invalidTypeRow = makeRowVector({timestampKeyMap});
  VELOX_ASSERT_THROW(
      evaluateCast(
          ROW({MAP(TIMESTAMP(), BIGINT())}),
          JSON(),
          makeRowVector({invalidTypeRow})),
      "Cannot cast ROW<\"\":MAP<TIMESTAMP,BIGINT>> to JSON");

  // Map keys cannot be null.
  auto nullKeyVector =
      makeNullableFlatVector<StringView>({"red"_sv, std::nullopt});
  auto valueVector = makeNullableFlatVector<int64_t>({1, 2});

  auto offsets = allocateOffsets(1, pool());
  auto sizes = allocateSizes(1, pool());
  makeOffsetsAndSizes(2, 2, offsets, sizes);

  auto nullKeyMap = std::make_shared<MapVector>(
      pool(),
      MAP(VARCHAR(), BIGINT()),
      nullptr,
      1,
      offsets,
      sizes,
      nullKeyVector,
      valueVector);
  VELOX_ASSERT_THROW(
      evaluateCast(
          MAP(VARCHAR(), BIGINT()), JSON(), makeRowVector({nullKeyMap})),
      "Map keys cannot be null.");

  // Map keys cannot be complex type.
  auto arrayKeyVector = makeNullableArrayVector<int64_t>({{1}, {2}});
  auto arrayKeyMap = std::make_shared<MapVector>(
      pool(),
      MAP(ARRAY(BIGINT()), BIGINT()),
      nullptr,
      1,
      offsets,
      sizes,
      arrayKeyVector,
      valueVector);
  VELOX_ASSERT_THROW(
      evaluateCast(
          MAP(ARRAY(BIGINT()), BIGINT()), JSON(), makeRowVector({arrayKeyMap})),
      "Cannot cast MAP<ARRAY<BIGINT>,BIGINT> to JSON");

  // Map keys of json type must not be null.
  auto jsonKeyVector =
      makeNullableFlatVector<JsonNativeType>({"red"_sv, std::nullopt}, JSON());
  auto invalidJsonKeyMap = std::make_shared<MapVector>(
      pool(),
      MAP(JSON(), BIGINT()),
      nullptr,
      1,
      offsets,
      sizes,
      jsonKeyVector,
      valueVector);
  VELOX_ASSERT_THROW(
      evaluateCast(
          MAP(JSON(), BIGINT()), JSON(), makeRowVector({invalidJsonKeyMap})),
      "Cannot cast map with null keys to JSON");
}

TEST_F(JsonCastTest, toVarchar) {
  testCast<JsonNativeType, StringView>(
      JSON(),
      VARCHAR(),
      {R"("aaa")"_sv, R"("bbb")"_sv, R"("ccc")"_sv, R"("")"_sv},
      {"aaa"_sv, "bbb"_sv, "ccc"_sv, ""_sv});
  testCast<JsonNativeType, StringView>(
      JSON(),
      VARCHAR(),
      {"\"\""_sv,
       std::nullopt,
       R"("\u0001\u0002\u0003\u0004\u0005\u0006\u0007\b\t\n\u000b\f\r\u000e\u000f\u0010\u0011\u0012\u0013\u0014\u0015\u0016\u0017\u0018\u0019\u001a\u001b\u001c\u001d\u001e\u001f\"\\ .")"_sv},
      {""_sv,
       std::nullopt,
       "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f\"\\ ."_sv});
  testCast<JsonNativeType, StringView>(
      JSON(),
      VARCHAR(),
      {"123"_sv, "-12.3"_sv, "true"_sv, "false"_sv, "null"_sv},
      {"123"_sv, "-12.3"_sv, "true"_sv, "false"_sv, std::nullopt});
  testCast<JsonNativeType, StringView>(
      JSON(),
      VARCHAR(),
      {"null"_sv, std::nullopt},
      {std::nullopt, std::nullopt});
}

TEST_F(JsonCastTest, toInteger) {
  testCast<JsonNativeType, int64_t>(
      JSON(),
      BIGINT(),
      {"1"_sv,
       "-3"_sv,
       "0"_sv,
       "9223372036854775807"_sv,
       "-9223372036854775808"_sv,
       std::nullopt},
      {1, -3, 0, INT64_MAX, INT64_MIN, std::nullopt});
  testCast<JsonNativeType, int8_t>(
      JSON(),
      TINYINT(),
      {"1"_sv,
       "-3"_sv,
       "0"_sv,
       "127"_sv,
       "-128"_sv,
       "true"_sv,
       "false"_sv,
       "10.23"_sv,
       "-10.23"_sv,
       std::nullopt},
      {1, -3, 0, INT8_MAX, INT8_MIN, 1, 0, 10, -10, std::nullopt});
  testCast<JsonNativeType, int32_t>(
      JSON(),
      INTEGER(),
      {"null"_sv, std::nullopt},
      {std::nullopt, std::nullopt});

  testThrow<JsonNativeType>(
      JSON(),
      TINYINT(),
      {"128"_sv},
      "The JSON number is too large or too small to fit within the requested type");
  testThrow<JsonNativeType>(
      JSON(),
      TINYINT(),
      {"128.01"_sv},
      "The JSON number is too large or too small to fit within the requested type");
  testThrow<JsonNativeType>(
      JSON(),
      TINYINT(),
      {"-1223456"_sv},
      "The JSON number is too large or too small to fit within the requested type");
  testThrow<JsonNativeType>(
      JSON(),
      TINYINT(),
      {"\"Infinity\""_sv},
      "The JSON element does not have the requested type");
  testThrow<JsonNativeType>(
      JSON(),
      TINYINT(),
      {"\"NaN\""_sv},
      "The JSON element does not have the requested type");
  testThrow<JsonNativeType>(JSON(), TINYINT(), {""_sv}, "no JSON found");
  testThrow<JsonNativeType>(
      JSON(),
      BIGINT(),
      {"233897314173811950000"_sv},
      "Problem while parsing a number");
}

TEST_F(JsonCastTest, toDouble) {
  testCast<JsonNativeType, double>(
      JSON(),
      DOUBLE(),
      {"1.1"_sv,
       "2.0001"_sv,
       "10"_sv,
       "3.14e-2"_sv,
       "123"_sv,
       "true"_sv,
       "false"_sv,
       R"("Infinity")"_sv,
       R"("-Infinity")"_sv,
       R"("NaN")"_sv,
       R"("-NaN")"_sv,
       "233897314173811950000"_sv,
       std::nullopt},
      {1.1,
       2.0001,
       10.0,
       0.0314,
       123,
       1,
       0,
       HUGE_VAL,
       -HUGE_VAL,
       std::numeric_limits<double>::quiet_NaN(),
       std::numeric_limits<double>::quiet_NaN(),
       233897314173811950000.0,
       std::nullopt});
  testCast<JsonNativeType, double>(
      JSON(),
      DOUBLE(),
      {"null"_sv, std::nullopt},
      {std::nullopt, std::nullopt});

  testThrow<JsonNativeType>(
      JSON(),
      REAL(),
      {"-1.7E+307"_sv},
      "The JSON number is too large or too small to fit within the requested type");
  testThrow<JsonNativeType>(
      JSON(),
      REAL(),
      {"1.7E+307"_sv},
      "The JSON number is too large or too small to fit within the requested type");
  testThrow<JsonNativeType>(JSON(), REAL(), {""_sv}, "no JSON found");
  testThrow<JsonNativeType>(
      JSON(),
      DOUBLE(),
      {"Infinity"_sv},
      "The JSON document has an improper structure");
  testThrow<JsonNativeType>(
      JSON(),
      DOUBLE(),
      {"NaN"_sv},
      "The JSON document has an improper structure");
}

TEST_F(JsonCastTest, toBoolean) {
  testCast<JsonNativeType, bool>(
      JSON(),
      BOOLEAN(),
      {"true"_sv,
       "false"_sv,
       R"("true")"_sv,
       R"("false")"_sv,
       "123"_sv,
       "-123"_sv,
       "0.56"_sv,
       "-0.56"_sv,
       "0"_sv,
       "0.0"_sv,
       std::nullopt},
      {true,
       false,
       true,
       false,
       true,
       true,
       true,
       true,
       false,
       false,
       std::nullopt});
  testCast<JsonNativeType, bool>(
      JSON(),
      BOOLEAN(),
      {"null"_sv, std::nullopt},
      {std::nullopt, std::nullopt});

  testThrow<JsonNativeType>(
      JSON(),
      BOOLEAN(),
      {R"("123")"_sv},
      "The JSON element does not have the requested type");
  testThrow<JsonNativeType>(
      JSON(),
      BOOLEAN(),
      {R"("abc")"_sv},
      "The JSON element does not have the requested type");
  testThrow<JsonNativeType>(JSON(), BOOLEAN(), {""_sv}, "no JSON found");
}

TEST_F(JsonCastTest, toArray) {
  auto data = makeNullableFlatVector<JsonNativeType>(
      {R"(["red","blue"])"_sv,
       R"([null,null,"purple"])"_sv,
       "[]"_sv,
       "null"_sv},
      JSON());
  auto expected = makeNullableArrayVector<StringView>(
      {{{"red"_sv, "blue"_sv}},
       {{std::nullopt, std::nullopt, "purple"_sv}},
       {{}},
       std::nullopt});

  testCast(data, expected);

  // Tests array that has null at every row.
  data = makeNullableFlatVector<JsonNativeType>(
      {"null"_sv, "null"_sv, "null"_sv, "null"_sv, std::nullopt}, JSON());
  expected = makeNullableArrayVector<int64_t>(
      {std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt});

  testCast(data, expected);

  data = makeNullableFlatVector<JsonNativeType>(
      {"[233897314173811950000]"_sv}, JSON());
  expected = makeArrayVector<double>({{233897314173811950000.0}});
  testCast(data, expected);
}

TEST_F(JsonCastTest, toMap) {
  auto data = makeNullableFlatVector<JsonNativeType>(
      {R"({"red":"1","blue":2.2})"_sv,
       R"({"purple":null,"yellow":4})"_sv,
       "{}"_sv,
       "null"_sv},
      JSON());
  auto expected = makeNullableMapVector<StringView, StringView>(
      {{{{"blue"_sv, "2.2"_sv}, {"red"_sv, "1"_sv}}},
       {{{"purple"_sv, std::nullopt}, {"yellow"_sv, "4"_sv}}},
       {{}},
       std::nullopt});

  testCast(data, expected);

  // Tests map of non-string keys.
  data = makeNullableFlatVector<JsonNativeType>(
      {R"({"102":"2","101":1.1})"_sv,
       R"({"103":null,"104":4})"_sv,
       "{}"_sv,
       "null"_sv},
      JSON());
  expected = makeNullableMapVector<int64_t, double>(
      {{{{101, 1.1}, {102, 2.0}}},
       {{{103, std::nullopt}, {104, 4.0}}},
       {{}},
       std::nullopt});

  testCast(data, expected);

  // Tests map that has null at every row.
  data = makeNullableFlatVector<JsonNativeType>(
      {"null"_sv, "null"_sv, "null"_sv, "null"_sv, std::nullopt}, JSON());
  expected = makeNullableMapVector<StringView, int64_t>(
      {std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt});

  testCast(data, expected);

  // Null keys or non-string keys in JSON maps are not allowed.
  testThrow<JsonNativeType>(
      JSON(),
      MAP(VARCHAR(), DOUBLE()),
      {R"({"red":1.1,"blue":2.2})"_sv, R"({null:3.3,"yellow":4.4})"_sv},
      "The JSON document has an improper structure");
  testThrow<JsonNativeType>(
      JSON(),
      MAP(BIGINT(), DOUBLE()),
      {"{1:1.1,2:2.2}"_sv},
      "The JSON document has an improper structure");
}

TEST_F(JsonCastTest, orderOfKeys) {
  auto data = makeFlatVector<JsonNativeType>(
      {
          R"({"k1": {"a": 1, "b": 2}})"_sv,
          R"({"k2": {"a": 10, "b": 20}})"_sv,
      },
      JSON());

  auto map = makeMapVector<std::string, JsonNativeType>(
      {
          {{"k1", R"({"a": 1, "b": 2})"}},
          {{"k2", R"({"a": 10, "b": 20})"}},
      },
      MAP(VARCHAR(), JSON()));

  testCast(data, map);
}

TEST_F(JsonCastTest, toRow) {
  // Test casting to ROW from JSON arrays.
  auto array = makeNullableFlatVector<JsonNativeType>(
      {R"([123,"abc",true])"_sv,
       R"([123,null,false])"_sv,
       R"([123,null,null])"_sv,
       R"([null,null,null])"_sv},
      JSON());
  auto child1 = makeNullableFlatVector<int64_t>({123, 123, 123, std::nullopt});
  auto child2 = makeNullableFlatVector<StringView>(
      {"abc"_sv, std::nullopt, std::nullopt, std::nullopt});
  auto child3 =
      makeNullableFlatVector<bool>({true, false, std::nullopt, std::nullopt});

  testCast(array, makeRowVector({child1, child2, child3}));

  // Test casting to ROW from JSON objects.
  auto map = makeNullableFlatVector<JsonNativeType>(
      {R"({"c0":123,"c1":"abc","c2":true})"_sv,
       R"({"c1":"abc","c2":true,"c0":123})"_sv,
       R"({"c0":123,"c2":true,"c0":456})"_sv,
       R"({"c3":123,"c4":"abc","c2":false})"_sv,
       R"({"c0":null,"c2":false})"_sv,
       R"({"c0":null,"c2":null,"c1":null})"_sv},
      JSON());
  auto child4 = makeNullableFlatVector<int64_t>(
      {123, 123, 456, std::nullopt, std::nullopt, std::nullopt});
  auto child5 = makeNullableFlatVector<StringView>(
      {"abc"_sv,
       "abc"_sv,
       std::nullopt,
       std::nullopt,
       std::nullopt,
       std::nullopt});
  auto child6 = makeNullableFlatVector<bool>(
      {true, true, true, false, false, std::nullopt});

  testCast(map, makeRowVector({child4, child5, child6}));

  // Use a mix of lower case and upper case JSON keys.
  map = makeNullableFlatVector<JsonNativeType>(
      {R"({"c0":123,"c1":"abc","c2":true})"_sv,
       R"({"c1":"abc","c2":true,"c0":123})"_sv,
       R"({"c0":123,"c2":true,"c0":456})"_sv,
       R"({"c3":123,"c4":"abc","c2":false})"_sv,
       R"({"c0":null,"c2":false})"_sv,
       R"({"c0":null,"c2":null,"c1":null})"_sv},
      JSON());
  testCast(map, makeRowVector({child4, child5, child6}));

  // Use a mix of lower case and upper case field names in target ROW type.
  testCast(map, makeRowVector({child4, child5, child6}));

  // Test casting to ROW from JSON null.
  auto null = makeNullableFlatVector<JsonNativeType>({"null"_sv}, JSON());
  auto nullExpected = makeRowVector(ROW({BIGINT(), DOUBLE()}), 1);
  nullExpected->setNull(0, true);

  testCast(null, nullExpected);
}

TEST_F(JsonCastTest, toNested) {
  auto array = makeNullableFlatVector<JsonNativeType>(
      {R"([[1,2],[3]])"_sv, R"([[null,null,4]])"_sv, "[[]]"_sv, "[]"_sv},
      JSON());
  auto arrayExpected = makeNullableNestedArrayVector<StringView>(
      {{{{{"1"_sv, "2"_sv}}, {{"3"_sv}}}},
       {{{{std::nullopt, std::nullopt, "4"_sv}}}},
       {{{{}}}},
       {{}}});

  testCast(array, arrayExpected);

  auto map = makeNullableFlatVector<JsonNativeType>(
      {R"({"1":[1.1,1.2],"2":[2,2.1]})"_sv, R"({"3":null,"4":[4.1,4.2]})"_sv},
      JSON());
  auto keys =
      makeNullableFlatVector<StringView>({"1"_sv, "2"_sv, "3"_sv, "4"_sv});
  auto innerArray = makeNullableArrayVector<double>(
      {{{1.1, 1.2}}, {{2.0, 2.1}}, std::nullopt, {{4.1, 4.2}}});

  auto offsets = allocateOffsets(2, pool());
  auto sizes = allocateSizes(2, pool());
  makeOffsetsAndSizes(4, 2, offsets, sizes);

  auto mapExpected = std::make_shared<MapVector>(
      pool(),
      MAP(VARCHAR(), ARRAY(DOUBLE())),
      nullptr,
      2,
      offsets,
      sizes,
      keys,
      innerArray);
  testCast(map, mapExpected);
}

TEST_F(JsonCastTest, toArrayAndMapOfJson) {
  // Test casting to array of JSON.
  auto array = makeNullableFlatVector<JsonNativeType>(
      {R"([[1,2],[null],null,"3"])"_sv, "[[]]"_sv, "[]"_sv}, JSON());
  auto arrayExpected = makeNullableArrayVector<StringView>(
      {{"[1,2]"_sv, "[null]"_sv, "null"_sv, "\"3\""_sv}, {"[]"_sv}, {}},
      ARRAY(JSON()));

  testCast(array, arrayExpected);

  // Test casting to map of JSON values.
  auto map = makeNullableFlatVector<JsonNativeType>(
      {R"({"k1":[1,23],"k2":456,"k3":null,"k4":"a"})"_sv,
       R"({"k5":{}})"_sv,
       "{}"_sv},
      JSON());
  auto mapExpected = makeMapVector<StringView, StringView>(
      {{{"k1"_sv, "[1,23]"_sv},
        {"k2"_sv, "456"_sv},
        {"k3"_sv, "null"_sv},
        {"k4"_sv, "\"a\""_sv}},
       {{"k5"_sv, "{}"_sv}},
       {}},
      MAP(VARCHAR(), JSON()));

  testCast(map, mapExpected);

  // The type of map keys is not allowed to be JSON.
  testThrow<JsonNativeType>(
      JSON(),
      MAP(JSON(), BIGINT()),
      {R"({"k1":1})"_sv},
      "Cannot cast JSON to MAP<JSON,BIGINT>");
}

TEST_F(JsonCastTest, toInvalid) {
  testThrow<JsonNativeType>(
      JSON(), TIMESTAMP(), {"null"_sv}, "Cannot cast JSON to TIMESTAMP");
  testThrow<JsonNativeType>(
      JSON(), DATE(), {"null"_sv}, "Cannot cast JSON to DATE");

  // Casting JSON arrays to ROW type with different number of fields or
  // unmatched field order is not allowed.
  testThrow<JsonNativeType>(
      JSON(),
      ROW({VARCHAR(), DOUBLE(), BIGINT()}),
      {R"(["red",1.1])"_sv, R"(["blue",2.2])"_sv},
      "The JSON element does not have the requested type");
  testThrow<JsonNativeType>(
      JSON(),
      ROW({VARCHAR()}),
      {R"(["red",1.1])"_sv, R"(["blue",2.2])"_sv},
      "The JSON element does not have the requested type");
  testThrow<JsonNativeType>(
      JSON(),
      ROW({DOUBLE(), VARCHAR()}),
      {R"(["red",1.1])"_sv, R"(["blue",2.2])"_sv},
      "The JSON element does not have the requested type");

  // Casting to ROW type from JSON text other than arrays or objects are not
  // supported.
  testThrow<JsonNativeType>(
      JSON(),
      ROW({BIGINT()}),
      {R"(123)"_sv, R"(456)"_sv},
      "The JSON element does not have the requested type");
}

TEST_F(JsonCastTest, castInTry) {
  // Test try(json as bigint)) whose input vector is wrapped in dictionary
  // encoding. The row of "1a" should trigger an error during casting and the
  // try expression should turn this error into a null at this row.
  auto input = makeRowVector(
      {makeFlatVector<JsonNativeType>({"1a"_sv, "2"_sv, "3"_sv}, JSON())});
  auto expected = makeNullableFlatVector<int64_t>({std::nullopt, 2, 3});

  evaluateAndVerifyCastInTryDictEncoding(JSON(), BIGINT(), input, expected);

  // Cast map whose elements are wrapped in a dictionary to Json. The map vector
  // contains four rows: {g -> null, null -> -6}, {e -> null, d -> -4}, {null ->
  // 3, b -> -2}, {null -> 1}.
  std::vector<std::optional<StringView>> keys{
      std::nullopt, "b"_sv, std::nullopt, "d"_sv, "e"_sv, std::nullopt, "g"_sv};
  std::vector<std::optional<int64_t>> values{1, -2, 3, -4, std::nullopt, -6, 7};
  auto map = makeMapWithDictionaryElements(keys, values, 2);

  auto jsonExpected = makeNullableFlatVector<JsonNativeType>(
      {std::nullopt, R"({"d":-4,"e":null})", std::nullopt, std::nullopt},
      JSON());
  evaluateAndVerifyCastInTryDictEncoding(
      MAP(VARCHAR(), BIGINT()), JSON(), makeRowVector({map}), jsonExpected);

  // Cast map vector that has null keys. The map vector contains three rows:
  // {blue -> 1, red -> 2}, {null -> 3, yellow -> 4}, {purple -> 5, null -> 6}.
  auto keyVector = makeNullableFlatVector<StringView>(
      {"blue"_sv,
       "red"_sv,
       std::nullopt,
       "yellow"_sv,
       "purple"_sv,
       std::nullopt},
      JSON());
  auto valueVector = makeNullableFlatVector<int64_t>({1, 2, 3, 4, 5, 6});

  auto mapOffsets = allocateOffsets(3, pool());
  auto mapSizes = allocateSizes(3, pool());
  makeOffsetsAndSizes(6, 2, mapOffsets, mapSizes);
  auto mapVector = std::make_shared<MapVector>(
      pool(),
      MAP(JSON(), BIGINT()),
      nullptr,
      3,
      mapOffsets,
      mapSizes,
      keyVector,
      valueVector);
  auto rowVector = makeRowVector({mapVector});

  jsonExpected = makeNullableFlatVector<JsonNativeType>(
      {"[{blue:1,red:2}]"_sv, std::nullopt, std::nullopt}, JSON());
  evaluateAndVerifyCastInTryDictEncoding(
      ROW({MAP(JSON(), BIGINT())}),
      JSON(),
      makeRowVector({rowVector}),
      jsonExpected);

  // Cast map whose elements are wrapped in constant encodings to Json.
  auto constantKey = BaseVector::wrapInConstant(6, 2, keyVector);
  auto constantValue = BaseVector::wrapInConstant(6, 3, valueVector);
  mapVector = std::make_shared<MapVector>(
      pool(),
      MAP(JSON(), BIGINT()),
      nullptr,
      3,
      mapOffsets,
      mapSizes,
      constantKey,
      constantValue);

  jsonExpected = makeNullableFlatVector<JsonNativeType>(
      {std::nullopt, std::nullopt, std::nullopt}, JSON());
  evaluateAndVerifyCastInTryDictEncoding(
      MAP(JSON(), BIGINT()), JSON(), makeRowVector({mapVector}), jsonExpected);

  // Cast array of map vector that has null keys. The array vector contains two
  // rows: [{blue -> 1, red -> 2}, {null -> 3, yellow -> 4}], [{purple -> 5,
  // null -> 6}].
  auto arrayOffsets = allocateOffsets(2, pool());
  auto arraySizes = allocateSizes(2, pool());
  makeOffsetsAndSizes(3, 2, arrayOffsets, arraySizes);
  auto arrayVector = std::make_shared<ArrayVector>(
      pool(),
      ARRAY(MAP(JSON(), BIGINT())),
      nullptr,
      2,
      arrayOffsets,
      arraySizes,
      mapVector);
  rowVector = makeRowVector({arrayVector});

  jsonExpected = makeNullableFlatVector<JsonNativeType>(
      {std::nullopt, std::nullopt}, JSON());
  evaluateAndVerifyCastInTryDictEncoding(
      ROW({ARRAY(MAP(JSON(), BIGINT()))}),
      JSON(),
      makeRowVector({rowVector}),
      jsonExpected);
}

TEST_F(JsonCastTest, tryCastFromJson) {
  // Test try_cast to map when there are error in the conversions of map
  // elements.
  // To map(bigint, real).
  auto data = makeFlatVector<JsonNativeType>(
      {R"({"102":"2","101a":1.1})"_sv,
       R"({"103":null,"104":2859327816787296000})"_sv},
      JSON());
  std::vector<std::pair<int64_t, std::optional<float>>> row;
  row.emplace_back(103, std::nullopt);
  row.emplace_back(104, 2859327816787296000.0f);
  auto expectedMap = makeNullableMapVector<int64_t, float>({std::nullopt, row});
  evaluateAndVerify(
      JSON(), MAP(BIGINT(), REAL()), makeRowVector({data}), expectedMap, true);

  // To array(bigint).
  data = makeFlatVector<JsonNativeType>(
      {R"(["102a","101a"])"_sv, R"(["103a","2859327816787296000"])"_sv},
      JSON());
  auto expectedArray =
      makeNullableArrayVector<float>({std::nullopt, std::nullopt});
  evaluateAndVerify(
      JSON(), ARRAY(REAL()), makeRowVector({data}), expectedArray, true);

  // To row(bigint).
  data = makeFlatVector<JsonNativeType>(
      {R"(["101a"])"_sv, R"(["28593278167872960000000a"])"_sv}, JSON());
  auto expectedRow = makeRowVector(
      {makeFlatVector<float>({0, 0})}, [](auto /*row*/) { return true; });
  evaluateAndVerify(
      JSON(), ROW({REAL()}), makeRowVector({data}), expectedRow, true);

  // To primitive.
  data = makeFlatVector<JsonNativeType>(
      {R"("101a")"_sv, R"("28593278167872960000000a")"_sv}, JSON());
  auto expected = makeNullableFlatVector<float>({std::nullopt, std::nullopt});
  evaluateAndVerify(JSON(), REAL(), makeRowVector({data}), expected, true);

  // Invalid input.
  data = makeFlatVector<JsonNativeType>(
      {R"(["101a"})"_sv, R"(["28593278167872960000000a"})"_sv}, JSON());
  expectedRow = makeRowVector(
      {makeFlatVector<float>({0, 0})}, [](auto /*row*/) { return true; });
  evaluateAndVerify(
      JSON(), ROW({REAL()}), makeRowVector({data}), expectedRow, true);
}
