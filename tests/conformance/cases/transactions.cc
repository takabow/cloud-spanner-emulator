//
// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "zetasql/base/testing/status_matchers.h"
#include "tests/common/proto_matchers.h"
#include "absl/status/status.h"
#include "absl/time/clock.h"
#include "google/cloud/spanner/mutations.h"
#include "google/cloud/spanner/timestamp.h"
#include "google/cloud/spanner/transaction.h"
#include "tests/conformance/common/database_test_base.h"
#include "absl/status/status.h"

namespace google {
namespace spanner {
namespace emulator {
namespace test {

namespace {

using cloud::spanner::InsertMutationBuilder;
using google::cloud::spanner::internal::MakeSingleUseTransaction;
using zetasql_base::testing::IsOk;
using zetasql_base::testing::StatusIs;

class TransactionsTest : public DatabaseTest {
 public:
  absl::Status SetUpDatabase() override {
    return SetSchema({R"(
      CREATE TABLE TestTable(
        key1 STRING(MAX) NOT NULL,
        key2 STRING(MAX),
        col1 STRING(MAX)
      ) PRIMARY KEY (key1, key2)
    )"});
  }
};

TEST_F(TransactionsTest, SingleUseReadOnlyTransactionCannotBeCommitted) {
  auto txn = MakeSingleUseTransaction(
      Transaction::SingleUseOptions{Transaction::ReadOnlyOptions{}});
  EXPECT_THAT(CommitTransaction(txn, {}),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_F(TransactionsTest, SingleUseReadOnlyTransactionCannotBeRolledBack) {
  auto txn = MakeSingleUseTransaction(
      Transaction::SingleUseOptions{Transaction::ReadOnlyOptions{}});
  EXPECT_THAT(Rollback(txn), StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(TransactionsTest, ReadOnlyTransactionCannotBeRolledBack) {
  auto txn = Transaction(Transaction::ReadOnlyOptions());
  {
    auto result =
        Read(txn, "TestTable", {"key1", "key2", "col1"}, KeySet::All());
    EXPECT_THAT(result.status(), IsOk());
    EXPECT_THAT(result->values, testing::ElementsAre());
    EXPECT_THAT(result->has_read_timestamp, true);
  }
  EXPECT_THAT(Rollback(txn), StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_F(TransactionsTest, ReadOnlyTransactionCannotBeCommitted) {
  auto txn = Transaction(Transaction::ReadOnlyOptions());
  EXPECT_THAT(CommitTransaction(txn, {}),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_F(TransactionsTest, SingleUseReadOnlyTimestampMustBeValid) {
  ZETASQL_ASSERT_OK_AND_ASSIGN(CommitResult result,
                       Insert("TestTable", {"key1", "key2", "col1"},
                              {"value1", "value2", "value3"}));

  EXPECT_THAT(Read(Transaction::SingleUseOptions(MakeMinTimestamp()),
                   "TestTable", {"key1", "key2", "col1"}, KeySet::All()),
              StatusIs(absl::StatusCode::kInvalidArgument));

  EXPECT_THAT(Read(Transaction::SingleUseOptions(result.commit_timestamp),
                   "TestTable", {"key1", "key2", "col1"}, KeySet::All()),
              IsOkAndHoldsRow({ValueRow{"value1", "value2", "value3"}}));
}

TEST_F(TransactionsTest, ReadOnlyTimestampMustBeValid) {
  ZETASQL_ASSERT_OK_AND_ASSIGN(CommitResult commit_result,
                       Insert("TestTable", {"key1", "key2", "col1"},
                              {"value1", "value2", "value3"}));

  auto min_txn = Transaction(Transaction::ReadOnlyOptions(MakeMinTimestamp()));
  EXPECT_THAT(
      Read(min_txn, "TestTable", {"key1", "key2", "col1"}, KeySet::All()),
      StatusIs(absl::StatusCode::kInvalidArgument));

  auto now_txn =
      Transaction(Transaction::ReadOnlyOptions(commit_result.commit_timestamp));
  auto result =
      Read(now_txn, "TestTable", {"key1", "key2", "col1"}, KeySet::All());
  EXPECT_THAT(result.status(), IsOk());
  EXPECT_THAT(result->values,
              testing::ElementsAre(ValueRow{"value1", "value2", "value3"}));
}

TEST_F(TransactionsTest, ReadOnlyTransactionCheckReadTimestamp) {
  ZETASQL_ASSERT_OK_AND_ASSIGN(CommitResult commit_result,
                       Insert("TestTable", {"key1", "key2", "col1"},
                              {"value1", "value2", "value3"}));

  auto read_only_txn =
      Transaction(Transaction::ReadOnlyOptions(commit_result.commit_timestamp));
  auto result =
      Read(read_only_txn, "TestTable", {"key1", "key2", "col1"}, KeySet::All());
  EXPECT_THAT(result.status(), IsOk());
  EXPECT_THAT(result->values,
              testing::ElementsAre(ValueRow{"value1", "value2", "value3"}));
  EXPECT_THAT(result->has_read_timestamp, true);
}

TEST_F(TransactionsTest, ReadWriteTransactionRollbackReplayIsOk) {
  auto txn = Transaction(Transaction::ReadWriteOptions());
  {
    auto result =
        Read(txn, "TestTable", {"key1", "key2", "col1"}, KeySet::All());
    EXPECT_THAT(result.status(), IsOk());
    EXPECT_THAT(result->values, testing::ElementsAre());
    EXPECT_THAT(result->has_read_timestamp, false);
  }
  ZETASQL_EXPECT_OK(Rollback(txn));
  ZETASQL_EXPECT_OK(Rollback(txn));
}

TEST_F(TransactionsTest, ReadWriteTransactionInvalidatedAfterError) {
  auto txn = Transaction(Transaction::ReadWriteOptions());
  // Invalid mutation since all values are not present for the given columns.
  auto invalid_mutation =
      InsertMutationBuilder("TestTable", {"key1", "key2", "col1"})
          .AddRow({Value("val1"), Value("val2")})
          .Build();
  auto valid_mutation =
      InsertMutationBuilder("TestTable", {"key1", "key2", "col1"})
          .AddRow({Value("val1"), Value("val2"), Value("val3")})
          .Build();

  // An error returned from commit due to invalid mutation.
  EXPECT_THAT(CommitTransaction(txn, {invalid_mutation}),
              zetasql_base::testing::StatusIs(absl::StatusCode::kInvalidArgument));
  // Second attempt to commit should replay the same error even with a valid
  // mutation.
  EXPECT_THAT(CommitTransaction(txn, {valid_mutation}),
              zetasql_base::testing::StatusIs(absl::StatusCode::kInvalidArgument));

  // Cannot continue to use this transaction for subsequent Read / Write.
  auto result = Read(txn, "TestTable", {"key1", "key2", "col1"}, KeySet::All());
  EXPECT_THAT(
      result.status(),
      (in_prod_env()
           ? zetasql_base::testing::StatusIs(absl::StatusCode::kFailedPrecondition)
           : zetasql_base::testing::StatusIs(absl::StatusCode::kInvalidArgument)));
}

TEST_F(TransactionsTest, ReadWriteTransactionCannotCommitWithNonExistentTable) {
  auto txn = Transaction(Transaction::ReadWriteOptions());
  auto mutation =
      InsertMutationBuilder("non_existent_table", {"key1", "key2", "col1"})
          .AddRow({Value("val1"), Value("val2"), Value("val3")})
          .Build();

  // This returns an error since it can't find the table in the mutation.
  EXPECT_THAT(CommitTransaction(txn, {mutation}),
              zetasql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(TransactionsTest, ReadWriteTransactionCanCommitAfterNotFoundRead) {
  auto txn = Transaction(Transaction::ReadWriteOptions());
  auto mutation = InsertMutationBuilder("TestTable", {"key1", "key2", "col1"})
                      .AddRow({Value("val1"), Value("val2"), Value("val3")})
                      .Build();

  auto result =
      Read(txn, "non_existent_table", {"key1", "key2"}, KeySet::All());
  EXPECT_THAT(result.status(),
              zetasql_base::testing::StatusIs(absl::StatusCode::kNotFound));

  // This succeeds since the previous read does not mark the transaction as
  // invalid.
  ZETASQL_EXPECT_OK(CommitTransaction(txn, {mutation}));
}

TEST_F(TransactionsTest, ReadWriteTransactionCannotCommitAfterNotFoundCommit) {
  auto txn = Transaction(Transaction::ReadWriteOptions());
  auto invalid_mutation =
      InsertMutationBuilder("non_existent_table", {"key1", "key2", "col1"})
          .AddRow({Value("val1"), Value("val2"), Value("val3")})
          .Build();
  auto valid_mutation =
      InsertMutationBuilder("TestTable", {"key1", "key2", "col1"})
          .AddRow({Value("val1"), Value("val2"), Value("val3")})
          .Build();

  // This returns an error since it can't find the table.
  EXPECT_THAT(CommitTransaction(txn, {invalid_mutation}),
              zetasql_base::testing::StatusIs(absl::StatusCode::kNotFound));

  // This replays the previous error status since the transaction has been
  // invalidated.
  EXPECT_THAT(CommitTransaction(txn, {valid_mutation}),
              zetasql_base::testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(TransactionsTest, FailedMutationReleasesTransactionLocks) {
  // Invalid mutation and expected to fail.
  auto mutation = InsertMutationBuilder("TestTable", {"key1", "key2", "col1"})
                      .AddRow({Value("val1"), Value("val2")})
                      .Build();

  // An error returned from commit due to invalid mutation.
  EXPECT_THAT(Commit({mutation}),
              zetasql_base::testing::StatusIs(absl::StatusCode::kInvalidArgument));

  // Subsequent transactions should succeed.
  ZETASQL_ASSERT_OK(CommitDml(
      {SqlStatement("INSERT TestTable(key1, key2, col1) Values ('val1', "
                    "'val2', 'value')"),
       SqlStatement("UPDATE TestTable SET col1 = 'new-value' WHERE key1 = "
                    "'val1' AND key2 = 'val2'")}));
  EXPECT_THAT(Query("SELECT * FROM TestTable"),
              IsOkAndHoldsRows({{"val1", "val2", "new-value"}}));
}

TEST_F(TransactionsTest, FailedDmlReleasesTransactionLocks) {
  // This is a malformed dml and expected to fail.
  EXPECT_THAT(CommitDml({SqlStatement("DELETE * FROM TestTable")}),
              StatusIs(absl::StatusCode::kInvalidArgument));

  // Subsequent transactions should succeed.
  ZETASQL_ASSERT_OK(CommitDml(
      {SqlStatement("INSERT TestTable(key1, key2, col1) Values ('val1', "
                    "'val2', 'value')"),
       SqlStatement("UPDATE TestTable SET col1 = 'new-value' WHERE key1 = "
                    "'val1' AND key2 = 'val2'")}));
  EXPECT_THAT(Query("SELECT * FROM TestTable"),
              IsOkAndHoldsRows({{"val1", "val2", "new-value"}}));
}

TEST_F(TransactionsTest, ReadWriteTransactionCannotRollbackAfterCommit) {
  auto txn = Transaction(Transaction::ReadWriteOptions());
  {
    auto result =
        Read(txn, "TestTable", {"key1", "key2", "col1"}, KeySet::All());
    EXPECT_THAT(result.status(), IsOk());
    EXPECT_THAT(result->values, testing::ElementsAre());
    EXPECT_THAT(result->has_read_timestamp, false);
  }
  auto mutation = InsertMutationBuilder("TestTable", {"key1", "key2", "col1"})
                      .AddRow({Value("val1"), Value("val2"), Value("val3")})
                      .Build();
  ZETASQL_EXPECT_OK(CommitTransaction(txn, {mutation}));
  EXPECT_THAT(Rollback(txn), StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_F(TransactionsTest, ReadWriteTransactionCannotCommitAfterRollback) {
  auto txn = Transaction(Transaction::ReadWriteOptions());
  {
    auto result =
        Read(txn, "TestTable", {"key1", "key2", "col1"}, KeySet::All());
    EXPECT_THAT(result.status(), IsOk());
    EXPECT_THAT(result->values, testing::ElementsAre());
    EXPECT_THAT(result->has_read_timestamp, false);
  }
  ZETASQL_EXPECT_OK(Rollback(txn));
  auto mutation = InsertMutationBuilder("TestTable", {"key1", "key2", "col1"})
                      .AddRow({Value("val1"), Value("val2"), Value("val3")})
                      .Build();
  EXPECT_THAT(CommitTransaction(txn, {mutation}),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_F(TransactionsTest, ReadWriteTransactionCannotReadAfterCommit) {
  auto txn = Transaction(Transaction::ReadWriteOptions());
  {
    auto result =
        Read(txn, "TestTable", {"key1", "key2", "col1"}, KeySet::All());
    EXPECT_THAT(result.status(), IsOk());
    EXPECT_THAT(result->values, testing::ElementsAre());
    EXPECT_THAT(result->has_read_timestamp, false);
  }
  auto mutation = InsertMutationBuilder("TestTable", {"key1", "key2", "col1"})
                      .AddRow({Value("val1"), Value("val2"), Value("val3")})
                      .Build();
  ZETASQL_EXPECT_OK(CommitTransaction(txn, {mutation}));
  EXPECT_THAT(Read(txn, "TestTable", {"key1", "key2", "col1"}, KeySet::All()),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_F(TransactionsTest, ReadWriteTransactionCannotReadAfterRollback) {
  auto txn = Transaction(Transaction::ReadWriteOptions());
  {
    auto result =
        Read(txn, "TestTable", {"key1", "key2", "col1"}, KeySet::All());
    EXPECT_THAT(result.status(), IsOk());
    EXPECT_THAT(result->values, testing::ElementsAre());
    EXPECT_THAT(result->has_read_timestamp, false);
  }
  ZETASQL_EXPECT_OK(Rollback(txn));
  EXPECT_THAT(Read(txn, "TestTable", {"key1", "key2", "col1"}, KeySet::All()),
              StatusIs(absl::StatusCode::kFailedPrecondition));
}

TEST_F(TransactionsTest, StrongReadSeesLastCommitTimestamp) {
  ZETASQL_ASSERT_OK_AND_ASSIGN(
      auto commit_result,
      Commit({MakeInsert("TestTable", {"key1", "key2"}, "val1", "val2")}));

  ZETASQL_ASSERT_OK_AND_ASSIGN(
      auto read_result,
      Read("TestTable", {"key1", "key2"}, KeySet::All(),
           Transaction::SingleUseOptions{Transaction::ReadOnlyOptions{}}));
  EXPECT_LE(commit_result.commit_timestamp, read_result.read_timestamp);
  EXPECT_THAT(read_result.values,
              testing::ElementsAre(ValueRow{"val1", "val2"}));
}

TEST_F(TransactionsTest, QueryWithBoundedStalenessDoesNotSeeOldValues) {
  ZETASQL_ASSERT_OK_AND_ASSIGN(auto commit_result,
                       Commit({MakeInsert("TestTable", {"key1", "key2", "col1"},
                                          "key1", "key2", "val1")}));
  ZETASQL_ASSERT_OK(Commit({MakeUpdate("TestTable", {"key1", "key2", "col1"}, "key1",
                               "key2", "val2")}));
  EXPECT_THAT(QuerySingleUseTransaction(
                  Transaction::SingleUseOptions(commit_result.commit_timestamp),
                  SqlStatement("SELECT col1 FROM TestTable")),
              IsOkAndHoldsRows({{"val2"}}));
}

}  // namespace

}  // namespace test
}  // namespace emulator
}  // namespace spanner
}  // namespace google
