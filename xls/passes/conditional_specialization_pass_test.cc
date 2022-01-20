// Copyright 2021 The XLS Authors
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

#include "xls/passes/conditional_specialization_pass.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/statusor.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/ir/function.h"
#include "xls/ir/ir_matcher.h"
#include "xls/ir/ir_test_base.h"
#include "xls/ir/package.h"

namespace m = ::xls::op_matchers;

namespace xls {
namespace {

using status_testing::IsOkAndHolds;

class ConditionalSpecializationPassTest : public IrTestBase {
 protected:
  absl::StatusOr<bool> Run(Function* f) {
    PassResults results;
    XLS_ASSIGN_OR_RETURN(bool changed,
                         ConditionalSpecializationPass().RunOnFunctionBase(
                             f, PassOptions(), &results));
    return changed;
  }
};

TEST_F(ConditionalSpecializationPassTest, SpecializeSelectSimple) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, ParseFunction(R"(
fn f(a: bits[1], b: bits[31], z: bits[32]) -> bits[32] {
  concat: bits[32] = concat(a, b)
  ret sel.2: bits[32] = sel(a, cases=[z, concat])
}
  )",
                                                       p.get()));
  EXPECT_THAT(Run(f), IsOkAndHolds(true));
  EXPECT_THAT(
      f->return_value(),
      m::Select(m::Param("a"),
                {m::Param("z"), m::Concat(m::Literal(1), m::Param("b"))}));
}

TEST_F(ConditionalSpecializationPassTest, SpecializeSelectMultipleBranches) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, ParseFunction(R"(
fn f(a: bits[32], x: bits[32], y: bits[32], z: bits[32]) -> bits[32] {
  add.1: bits[32] = add(a, x)
  add.2: bits[32] = add(a, y)
  add.3: bits[32] = add(a, z)
  ret sel.4: bits[32] = sel(a, cases=[add.1, add.2, add.3], default=a)
}
  )",
                                                       p.get()));
  EXPECT_THAT(Run(f), IsOkAndHolds(true));
  EXPECT_THAT(f->return_value(),
              m::Select(m::Param("a"),
                        {m::Add(m::Literal(0), m::Param("x")),
                         m::Add(m::Literal(1), m::Param("y")),
                         m::Add(m::Literal(2), m::Param("z"))},
                        m::Param("a")));
}

TEST_F(ConditionalSpecializationPassTest, SpecializeSelectSelectorExpression) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, ParseFunction(R"(
fn f(a: bits[32], x: bits[1]) -> bits[1] {
  literal.1: bits[32] = literal(value=7)
  ult.2: bits[1] = ult(a, literal.1)
  not.3: bits[1] = not(ult.2)
  ret sel.4: bits[1] = sel(ult.2, cases=[not.3, x])
}
  )",
                                                       p.get()));
  EXPECT_THAT(Run(f), IsOkAndHolds(true));
  EXPECT_THAT(f->return_value(),
              m::Select(m::ULt(m::Param("a"), m::Literal(7)),
                        {m::Not(m::Literal(0)), m::Param("x")}));
}

TEST_F(ConditionalSpecializationPassTest, SpecializeSelectNegative0) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, ParseFunction(R"(
fn f(a: bits[32], x: bits[32], y: bits[32]) -> bits[32] {
  not.1: bits[32] = not(a)
  add.2: bits[32] = add(not.1, x)
  add.3: bits[32] = add(not.1, y)
  ret sel.4: bits[32] = sel(a, cases=[add.2, add.3], default=a)
}
  )",
                                                       p.get()));
  // Select arm specialization does not apply because not(a) is used in both
  // branches:
  //
  //      not(a)
  //     /      \
  //   add.2   add.3
  //
  // This could be improved by making separate copies or each branch:
  //
  //   not(a)  not(a)
  //    |       |
  //   add.2   add.3
  //
  // and specializing the selector value separately for each:
  //
  //   not(0)  not(1)
  //    |        |
  //   add.2   add.3
  //
  EXPECT_THAT(Run(f), IsOkAndHolds(false));
}

TEST_F(ConditionalSpecializationPassTest, SpecializeSelectNegative1) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, ParseFunction(R"(
fn f(a: bits[32], x: bits[32], y: bits[32]) -> bits[32] {
  add.1: bits[32] = add(a, y)
  sel.2: bits[32] = sel(a, cases=[x, add.1], default=a)
  ret add.3: bits[32] = add(add.1, sel.2)
}
  )",
                                                       p.get()));
  // Similar to the negative test above, the select arm could be specialized
  // by creating a separate copy of the add.1 Node to be used in the return
  // value, and then replacing only the one used in the select arm.
  EXPECT_THAT(Run(f), IsOkAndHolds(false));
}

TEST_F(ConditionalSpecializationPassTest,
       SpecializeSelectWithDuplicateCaseArms) {
  // If an expression is used as more than one arm of the select it should not
  // be transformed because the same expression is used for multiple case
  // values.
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, ParseFunction(R"(
fn f(a: bits[32], y: bits[32]) -> bits[32] {
  add: bits[32] = add(a, y)
  ret sel: bits[32] = sel(a, cases=[add, add], default=a)
}
  )",
                                                       p.get()));
  EXPECT_THAT(Run(f), IsOkAndHolds(false));
}

TEST_F(ConditionalSpecializationPassTest, Consecutive2WaySelects) {
  //
  //  a   b                 a   b
  //   \ /                   \ /
  //   sel1 ----+-- p        sel1 ----- 0
  //    |       |       =>    |
  //    |  c    |             |  c
  //    | /     |             | /
  //   sel0 ----+            sel0 ----- p
  //    |                     |
  //
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  Type* u32 = p->GetBitsType(32);
  BValue a = fb.Param("a", u32);
  BValue b = fb.Param("b", u32);
  BValue c = fb.Param("c", u32);
  BValue pred = fb.Param("pred", p->GetBitsType(1));

  BValue sel1 = fb.Select(pred, {a, b});
  fb.Select(pred, {sel1, c});

  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());

  EXPECT_THAT(Run(f), IsOkAndHolds(true));

  EXPECT_THAT(
      f->return_value(),
      m::Select(m::Param("pred"),
                /*cases=*/{m::Select(m::Literal(0),
                                     /*cases=*/{m::Param("a"), m::Param("b")}),
                           m::Param("c")}));
}

TEST_F(ConditionalSpecializationPassTest, Consecutive2WaySelectsCase2) {
  //
  //    a   b               a   b
  //     \ /                 \ /
  //     sel1 -+-- p         sel1 ---- 1
  //      |    |              |
  //   c  |    |      =>   c  |
  //    \ |    |            \ |
  //     sel0 -+             sel0 ---- p
  //      |                   |
  //
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  Type* u32 = p->GetBitsType(32);
  BValue a = fb.Param("a", u32);
  BValue b = fb.Param("b", u32);
  BValue c = fb.Param("c", u32);
  BValue pred = fb.Param("pred", p->GetBitsType(1));

  BValue sel1 = fb.Select(pred, {a, b});
  fb.Select(pred, {c, sel1});

  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());

  EXPECT_THAT(Run(f), IsOkAndHolds(true));

  EXPECT_THAT(
      f->return_value(),
      m::Select(
          m::Param("pred"),
          /*cases=*/{m::Param("c"),
                     m::Select(m::Literal(1),
                               /*cases=*/{m::Param("a"), m::Param("b")})}));
}

TEST_F(ConditionalSpecializationPassTest, DuplicateArmSpecialization) {
  auto p = CreatePackage();
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, ParseFunction(R"(
fn f(s: bits[1], x: bits[8], y: bits[8]) -> bits[8] {
   sel0: bits[8] = sel(s, cases=[x,y])
   neg_sel0: bits[8] = neg(sel0)
   sel1: bits[8] = sel(s, cases=[neg_sel0, y])
   neg_sel1: bits[8] = neg(sel1)
   ret sel2: bits[8] = sel(s, cases=[neg_sel1, y])
}
  )",
                                                       p.get()));
  // 's' operand of sel0 can be specialized 0 due to sel1 *and* sel2 arm
  // specialization.  This should not cause a crash.
  EXPECT_THAT(Run(f), IsOkAndHolds(true));
  EXPECT_THAT(FindNode("sel0", f),
              m::Select(m::Literal(0), {m::Param("x"), m::Param("y")}));
}

TEST_F(ConditionalSpecializationPassTest, LongSelectChain) {
  // Build a long transformable select chain (first and last select share a
  // selector).
  //
  //  s0 = sel(s[0], cases=[a, b])
  //  s1 = sel(s[1], cases=[x[0], s0)
  //  s2 = sel(s[2], cases=[x[1], s1)
  //  ...
  //  s{n-1} = sel(s[n-1], cases=[x[n], s{n-2}])
  //  s{n} = sel(s[0], cases[x[n+1], s{n-1}])
  //
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  const int64_t kChainSize = 50;
  BValue s = fb.Param("s", p->GetBitsType(kChainSize));
  BValue x = fb.Param("x", p->GetBitsType(kChainSize + 1));
  BValue a = fb.Param("a", p->GetBitsType(1));
  BValue b = fb.Param("b", p->GetBitsType(1));
  BValue sel;
  for (int64_t i = 0; i <= kChainSize; ++i) {
    if (i == 0) {
      sel = fb.Select(fb.BitSlice(s, /*start=*/0, /*width=*/1), {a, b});
    } else if (i == kChainSize) {
      sel = fb.Select(fb.BitSlice(s, /*start=*/0, /*width=*/1),
                      {fb.BitSlice(x, /*start=*/i, /*width=*/1), sel});
    } else {
      sel = fb.Select(fb.BitSlice(s, /*start=*/i, /*width=*/1),
                      {fb.BitSlice(x, /*start=*/i, /*width=*/1), sel});
    }
  }

  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());

  EXPECT_THAT(Run(f), IsOkAndHolds(false));
}

TEST_F(ConditionalSpecializationPassTest, TooLongSelectChain) {
  // Build an otherwise transformable but too long select chain (first and last
  // select share a selector) but the chain is too long so the condition set
  // size is maxed out and the transformation doesn't occur.
  //
  //  s0 = sel(s[0], cases=[a, b])
  //  s1 = sel(s[1], cases=[x[0], s0)
  //  s2 = sel(s[2], cases=[x[1], s1)
  //  ...
  //  s{n-1} = sel(s[n-1], cases=[x[n], s{n-2}])
  //  s{n} = sel(s[0], cases[x[n+1], s{n-1}])
  //
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  const int64_t kChainSize = 100;
  BValue s = fb.Param("s", p->GetBitsType(kChainSize));
  BValue x = fb.Param("x", p->GetBitsType(kChainSize + 1));
  BValue a = fb.Param("a", p->GetBitsType(1));
  BValue b = fb.Param("b", p->GetBitsType(1));
  BValue sel;
  for (int64_t i = 0; i <= kChainSize; ++i) {
    if (i == 0) {
      sel = fb.Select(fb.BitSlice(s, /*start=*/0, /*width=*/1), {a, b});
    } else if (i == kChainSize) {
      sel = fb.Select(fb.BitSlice(s, /*start=*/0, /*width=*/1),
                      {fb.BitSlice(x, /*start=*/i, /*width=*/1), sel});
    } else {
      sel = fb.Select(fb.BitSlice(s, /*start=*/i, /*width=*/1),
                      {fb.BitSlice(x, /*start=*/i, /*width=*/1), sel});
    }
  }

  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());

  EXPECT_THAT(Run(f), IsOkAndHolds(false));
}

}  // namespace
}  // namespace xls