/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/optimizer/node.h"
#include "mongo/unittest/unittest.h"

namespace mongo::optimizer {
namespace {

TEST(Optimizer, Basic) {
    Context ctx;

    NodePtr ptrScan = ScanNode::create(ctx, "test");
    Node::ChildVector v;
    v.push_back(std::move(ptrScan));
    NodePtr ptrJoin = MultiJoinNode::create(ctx, {}, {}, std::move(v));
    ASSERT_EQ("NodeId: 1\nMultiJoin\nNodeId: 0\nScan\n", ptrJoin->generateMemo());

    NodePtr cloned = ptrJoin->clone(ctx);
    ASSERT_EQ("NodeId: 3\nMultiJoin\nNodeId: 2\nScan\n", cloned->generateMemo());
}

}  // namespace
}  // namespace mongo::optimizer
