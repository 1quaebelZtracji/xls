// Copyright 2022 The XLS Authors
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

#include "xls/passes/token_provenance_analysis.h"

#include <memory>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/types/span.h"
#include "xls/common/status/matchers.h"
#include "xls/data_structures/leaf_type_tree.h"
#include "xls/ir/bits.h"
#include "xls/ir/channel.h"
#include "xls/ir/channel_ops.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/ir_test_base.h"
#include "xls/ir/node.h"
#include "xls/ir/proc.h"
#include "xls/ir/value.h"

namespace xls {
namespace {

using testing::AllOf;
using testing::Contains;
using testing::ElementsAre;
using testing::IsEmpty;
using testing::Key;
using testing::Not;
using testing::Pair;
using testing::SizeIs;

class TokenProvenanceAnalysisTest : public IrTestBase {};

TEST_F(TokenProvenanceAnalysisTest, Simple) {
  auto p = CreatePackage();

  XLS_ASSERT_OK_AND_ASSIGN(
      StreamingChannel * channel,
      p->CreateStreamingChannel("test_channel", ChannelOps::kSendReceive,
                                p->GetBitsType(32)));

  ProcBuilder pb(TestName(), "token", p.get());
  pb.StateElement("state", Value(UBits(0, 0)));
  BValue recv = pb.Receive(channel, pb.GetTokenParam());
  BValue t1 = pb.TupleIndex(recv, 0);
  BValue t2 = pb.Send(channel, t1, pb.Literal(UBits(50, 32)));
  BValue tuple = pb.Tuple(
      {t1, pb.Literal(UBits(50, 32)),
       pb.Tuple({pb.Literal(UBits(50, 32)), pb.Literal(UBits(50, 32))}),
       pb.Tuple({t2})});
  BValue t3 = pb.Assert(pb.TupleIndex(pb.TupleIndex(tuple, 3), 0),
                        pb.Literal(UBits(1, 1)), "assertion failed");
  BValue t4 = pb.Trace(t3, pb.Literal(UBits(1, 1)), {}, "");
  BValue t5 = pb.Cover(t4, pb.Literal(UBits(1, 1)), "trace");
  BValue t6 = pb.AfterAll({t3, t4, t5});

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc,
                           pb.Build(t6, {pb.Literal(UBits(0, 0))}));
  XLS_ASSERT_OK_AND_ASSIGN(TokenProvenance provenance,
                           TokenProvenanceAnalysis(proc));

  EXPECT_EQ(provenance.at(pb.GetTokenParam().node()).Get({}),
            pb.GetTokenParam().node());
  EXPECT_EQ(provenance.at(recv.node()).Get({0}), recv.node());
  EXPECT_EQ(provenance.at(recv.node()).Get({1}), nullptr);
  EXPECT_EQ(provenance.at(tuple.node()).Get({0}), recv.node());
  EXPECT_EQ(provenance.at(tuple.node()).Get({1}), nullptr);
  EXPECT_EQ(provenance.at(tuple.node()).Get({2, 0}), nullptr);
  EXPECT_EQ(provenance.at(tuple.node()).Get({2, 1}), nullptr);
  EXPECT_EQ(provenance.at(tuple.node()).Get({3, 0}), t2.node());
  EXPECT_EQ(provenance.at(t3.node()).Get({}), t3.node());
  EXPECT_EQ(provenance.at(t4.node()).Get({}), t4.node());
  EXPECT_EQ(provenance.at(t5.node()).Get({}), t5.node());
  EXPECT_EQ(provenance.at(t6.node()).Get({}), t6.node());
}

TEST_F(TokenProvenanceAnalysisTest, VeryLongChain) {
  auto p = CreatePackage();
  ProcBuilder pb(TestName(), "token", p.get());
  BValue token = pb.GetTokenParam();
  for (int i = 0; i < 1000; ++i) {
    token = pb.Identity(token);
  }
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build(token, std::vector<BValue>()));
  XLS_ASSERT_OK_AND_ASSIGN(TokenProvenance provenance,
                           TokenProvenanceAnalysis(proc));

  // The proc only consists of a token param and token-typed identity
  // operations.
  for (Node* node : proc->nodes()) {
    EXPECT_EQ(provenance.at(node).Get({}), proc->TokenParam());
  }
}

TEST_F(TokenProvenanceAnalysisTest, TokenDAGSimple) {
  auto p = CreatePackage();

  XLS_ASSERT_OK_AND_ASSIGN(
      StreamingChannel * channel,
      p->CreateStreamingChannel("test_channel", ChannelOps::kSendReceive,
                                p->GetBitsType(32)));

  ProcBuilder pb(TestName(), "token", p.get());
  pb.StateElement("state", Value(UBits(0, 0)));
  BValue recv = pb.Receive(channel, pb.GetTokenParam());
  BValue t1 = pb.TupleIndex(recv, 0);
  BValue t2 = pb.Send(channel, t1, pb.Literal(UBits(50, 32)));
  BValue tuple = pb.Tuple(
      {t1, pb.Literal(UBits(50, 32)),
       pb.Tuple({pb.Literal(UBits(50, 32)), pb.Literal(UBits(50, 32))}),
       pb.Tuple({t2})});
  BValue t3 = pb.Assert(pb.TupleIndex(pb.TupleIndex(tuple, 3), 0),
                        pb.Literal(UBits(1, 1)), "assertion failed");
  BValue t4 = pb.Trace(t3, pb.Literal(UBits(1, 1)), {}, "");
  BValue t5 = pb.Cover(t4, pb.Literal(UBits(1, 1)), "trace");
  BValue t6 = pb.AfterAll({t3, t4, t5});

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc,
                           pb.Build(t6, {pb.Literal(UBits(0, 0))}));
  XLS_ASSERT_OK_AND_ASSIGN(TokenDAG dag, ComputeTokenDAG(proc));

  EXPECT_THAT(dag, Not(Contains(Key(proc->TokenParam()))));
  EXPECT_THAT(dag, AllOf(Contains(Key(recv.node())), Contains(Key(t2.node())),
                         Contains(Key(t3.node())), Contains(Key(t4.node())),
                         Contains(Key(t5.node())), Contains(Key(t6.node()))));
  EXPECT_THAT(dag.at(recv.node()), ElementsAre(proc->TokenParam()));
  EXPECT_THAT(dag.at(t2.node()), ElementsAre(recv.node()));
  EXPECT_THAT(dag.at(t3.node()), ElementsAre(t2.node()));
  EXPECT_THAT(dag.at(t4.node()), ElementsAre(t3.node()));
  EXPECT_THAT(dag.at(t5.node()), ElementsAre(t4.node()));
  EXPECT_THAT(dag.at(t6.node()),
              AllOf(SizeIs(3), Contains(t3.node()), Contains(t4.node()),
                    Contains(t5.node())));
}

TEST_F(TokenProvenanceAnalysisTest, TokenDAGVeryLongChain) {
  auto p = CreatePackage();
  ProcBuilder pb(TestName(), "token", p.get());
  BValue token = pb.GetTokenParam();
  for (int i = 0; i < 1000; ++i) {
    token = pb.Identity(token);
  }
  BValue assertion =
      pb.Assert(token, pb.Literal(UBits(1, 1)), {}, "assertion failed");
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc,
                           pb.Build(assertion, std::vector<BValue>()));
  XLS_ASSERT_OK_AND_ASSIGN(TokenDAG dag, ComputeTokenDAG(proc));
  EXPECT_THAT(dag, ElementsAre(Pair(assertion.node(),
                                    ElementsAre(proc->TokenParam()))));
}

TEST_F(TokenProvenanceAnalysisTest, TopoSortedTokenDAGSimple) {
  auto p = CreatePackage();

  XLS_ASSERT_OK_AND_ASSIGN(
      StreamingChannel * channel,
      p->CreateStreamingChannel("test_channel", ChannelOps::kSendReceive,
                                p->GetBitsType(32)));

  ProcBuilder pb(TestName(), "token", p.get());
  pb.StateElement("state", Value(UBits(0, 0)));
  BValue recv = pb.Receive(channel, pb.GetTokenParam());
  BValue t1 = pb.TupleIndex(recv, 0);
  BValue t2 = pb.Send(channel, t1, pb.Literal(UBits(50, 32)));
  BValue tuple = pb.Tuple(
      {t1, pb.Literal(UBits(50, 32)),
       pb.Tuple({pb.Literal(UBits(50, 32)), pb.Literal(UBits(50, 32))}),
       pb.Tuple({t2})});
  BValue t3 = pb.Assert(pb.TupleIndex(pb.TupleIndex(tuple, 3), 0),
                        pb.Literal(UBits(1, 1)), "assertion failed");
  BValue t4 = pb.Trace(t3, pb.Literal(UBits(1, 1)), {}, "");
  BValue t5 = pb.Cover(t4, pb.Literal(UBits(1, 1)), "trace");
  BValue t6 = pb.AfterAll({t3, t4, t5});

  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc,
                           pb.Build(t6, {pb.Literal(UBits(0, 0))}));

  XLS_ASSERT_OK_AND_ASSIGN(std::vector<NodeAndPredecessors> topo_dag,
                           ComputeTopoSortedTokenDAG(proc));

  EXPECT_EQ(topo_dag.size(), 7);
  EXPECT_EQ(topo_dag[0].node, proc->TokenParam());
  EXPECT_THAT(topo_dag[0].predecessors, IsEmpty());
  EXPECT_EQ(topo_dag[1].node, recv.node());
  EXPECT_THAT(topo_dag[1].predecessors, ElementsAre(proc->TokenParam()));
  EXPECT_EQ(topo_dag[2].node, t2.node());
  EXPECT_THAT(topo_dag[2].predecessors, ElementsAre(recv.node()));
  EXPECT_EQ(topo_dag[3].node, t3.node());
  EXPECT_THAT(topo_dag[3].predecessors, ElementsAre(t2.node()));
  EXPECT_EQ(topo_dag[4].node, t4.node());
  EXPECT_THAT(topo_dag[4].predecessors, ElementsAre(t3.node()));
  EXPECT_EQ(topo_dag[5].node, t5.node());
  EXPECT_THAT(topo_dag[5].predecessors, ElementsAre(t4.node()));
  EXPECT_EQ(topo_dag[6].node, t6.node());
  EXPECT_THAT(topo_dag[6].predecessors,
              AllOf(SizeIs(3), Contains(t3.node()), Contains(t4.node()),
                    Contains(t5.node())));
}

}  // namespace
}  // namespace xls
