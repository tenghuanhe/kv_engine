/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2020 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "item.h"
#include "kv_bucket.h"
#include "tests/mock/mock_dcp_consumer.h"
#include "tests/mock/mock_dcp_producer.h"
#include "tests/module_tests/collections/collections_dcp_test.h"
#include "tests/module_tests/collections/collections_test_helpers.h"
#include "tests/module_tests/test_helpers.h"

class CollectionsOSODcpTest : public CollectionsDcpTest {
public:
    CollectionsOSODcpTest() : CollectionsDcpTest() {
    }

    void SetUp() override {
        config_string += "collections_enabled=true";
        SingleThreadedKVBucketTest::SetUp();
        producers = std::make_unique<CollectionsDcpTestProducers>(engine.get());
        // Start vbucket as active to allow us to store items directly to it.
        store->setVBucketState(vbid, vbucket_state_active);
    }

    void testTwoCollections(bool backfillWillPause);

    std::pair<CollectionsManifest, uint64_t> setupTwoCollections();
};

std::pair<CollectionsManifest, uint64_t>
CollectionsOSODcpTest::setupTwoCollections() {
    VBucketPtr vb = store->getVBucket(vbid);
    CollectionsManifest cm(CollectionEntry::fruit);
    vb->updateFromManifest(makeManifest(cm.add(CollectionEntry::vegetable)));

    // Interleave the writes to two collections and then OSO backfill one
    store_item(vbid, makeStoredDocKey("b", CollectionEntry::fruit), "q");
    store_item(vbid, makeStoredDocKey("b", CollectionEntry::vegetable), "q");
    store_item(vbid, makeStoredDocKey("d", CollectionEntry::fruit), "a");
    store_item(vbid, makeStoredDocKey("d", CollectionEntry::vegetable), "q");
    store_item(vbid, makeStoredDocKey("a", CollectionEntry::fruit), "w");
    store_item(vbid, makeStoredDocKey("a", CollectionEntry::vegetable), "q");
    store_item(vbid, makeStoredDocKey("c", CollectionEntry::fruit), "y");
    store_item(vbid, makeStoredDocKey("c", CollectionEntry::vegetable), "q");
    flush_vbucket_to_disk(vbid, 10); // 8 keys + 2 events
    return {cm, 10};
}

// Run through how we expect OSO to work, this is a minimal test which will
// use the default collection
TEST_F(CollectionsOSODcpTest, basic) {
    // Write to default collection and deliberately not in lexicographical order
    store_item(vbid, makeStoredDocKey("b"), "q");
    store_item(vbid, makeStoredDocKey("d"), "a");
    store_item(vbid, makeStoredDocKey("a"), "w");
    store_item(vbid, makeStoredDocKey("c"), "y");
    flush_vbucket_to_disk(vbid, 4);

    // Reset so we have to stream from backfill
    resetEngineAndWarmup();

    // Filter on default collection (this will request from seqno:0)
    createDcpObjects({{R"({"collections":["0"]})"}}, true /* enable oso */);

    // We have a single filter, expect the backfill to be OSO
    runBackfill();

    // OSO snapshots are never really used in KV to KV replication, but this
    // test is using KV to KV test code, hence we need to set a snapshot so
    // that any transferred items don't trigger a snapshot exception.
    consumer->snapshotMarker(1, replicaVB, 0, 4, 0, 0, 4);

    // Manually step the producer and inspect all callbacks
    EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));
    EXPECT_EQ(cb::mcbp::ClientOpcode::DcpOsoSnapshot, producers->last_op);
    EXPECT_EQ(uint32_t(cb::mcbp::request::DcpOsoSnapshotFlags::Start),
              producers->last_oso_snapshot_flags);

    // We don't expect a collection create, this is the default collection which
    // clients assume exists unless deleted.
    std::array<std::string, 4> keys = {{"a", "b", "c", "d"}};
    for (auto& k : keys) {
        // Now we get the mutations, they aren't guaranteed to be in seqno
        // order, but we know that for now they will be in key order.
        EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));
        EXPECT_EQ(cb::mcbp::ClientOpcode::DcpMutation, producers->last_op);
        EXPECT_EQ(CollectionID::Default, producers->last_collection_id);
        EXPECT_EQ(k, producers->last_key);
    }

    // Now we get the end message
    EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));
    EXPECT_EQ(cb::mcbp::ClientOpcode::DcpOsoSnapshot, producers->last_op);
    EXPECT_EQ(uint32_t(cb::mcbp::request::DcpOsoSnapshotFlags::End),
              producers->last_oso_snapshot_flags);
}

void CollectionsOSODcpTest::testTwoCollections(bool backfillWillPause) {
    auto setup = setupTwoCollections();

    // Reset so we have to stream from backfill
    resetEngineAndWarmup();

    // Filter on vegetable collection (this will request from seqno:0)
    createDcpObjects({{R"({"collections":["a"]})"}}, true /* enable oso */);

    if (backfillWillPause) {
        producer->setBackfillBufferSize(1);
    }

    // We have a single filter, expect the backfill to be OSO
    runBackfill();

    // see comment in CollectionsOSODcpTest.basic
    consumer->snapshotMarker(1, replicaVB, 0, setup.second, 0, 0, setup.second);

    auto step = [this, &backfillWillPause]() {
        auto result = producer->stepWithBorderGuard(*producers);
        if (backfillWillPause) {
            // backfill paused, step does nothing
            EXPECT_EQ(ENGINE_EWOULDBLOCK, result);
            auto& lpAuxioQ = *task_executor->getLpTaskQ()[AUXIO_TASK_IDX];
            runNextTask(lpAuxioQ);
            EXPECT_EQ(ENGINE_SUCCESS,
                      producer->stepWithBorderGuard(*producers));
        } else {
            EXPECT_EQ(ENGINE_SUCCESS, result);
        }
    };

    // Manually step the producer and inspect all callbacks
    EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));
    EXPECT_EQ(cb::mcbp::ClientOpcode::DcpOsoSnapshot, producers->last_op);
    EXPECT_EQ(uint32_t(cb::mcbp::request::DcpOsoSnapshotFlags::Start),
              producers->last_oso_snapshot_flags);

    EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));
    EXPECT_EQ(cb::mcbp::ClientOpcode::DcpSystemEvent, producers->last_op);
    EXPECT_EQ(CollectionUid::vegetable, producers->last_collection_id);
    EXPECT_EQ("vegetable", producers->last_key);
    EXPECT_EQ(mcbp::systemevent::id::CreateCollection,
              producers->last_system_event);

    std::array<std::string, 4> keys = {{"a", "b", "c", "d"}};
    for (auto& k : keys) {
        // Now we get the mutations, they aren't guaranteed to be in seqno
        // order, but we know that for now they will be in key order.
        step();
        EXPECT_EQ(cb::mcbp::ClientOpcode::DcpMutation, producers->last_op);
        EXPECT_EQ(k, producers->last_key);
        EXPECT_EQ(CollectionUid::vegetable, producers->last_collection_id);
    }

    // Now we get the end message
    step();
    EXPECT_EQ(cb::mcbp::ClientOpcode::DcpOsoSnapshot, producers->last_op);
    EXPECT_EQ(uint32_t(cb::mcbp::request::DcpOsoSnapshotFlags::End),
              producers->last_oso_snapshot_flags);
}

TEST_F(CollectionsOSODcpTest, two_collections) {
    testTwoCollections(false);
}

TEST_F(CollectionsOSODcpTest, two_collections_backfill_pause) {
    testTwoCollections(true);
}

TEST_F(CollectionsOSODcpTest, dropped_collection) {
    auto setup = setupTwoCollections();

    // Reset so we have to stream from backfill
    resetEngineAndWarmup();

    // Filter on vegetable collection (this will request from seqno:0)
    createDcpObjects({{R"({"collections":["a"]})"}}, true /* enable oso */);

    // The drop is deliberately placed here, after we've permitted the stream
    // request to vegetable, yet before the stream schedules a backfill. So the
    // stream should only return a dropped vegetable event and no vegetable
    // items in the OSO snapshot
    VBucketPtr vb = store->getVBucket(vbid);
    vb->updateFromManifest(
            makeManifest(setup.first.remove(CollectionEntry::vegetable)));
    flush_vbucket_to_disk(vbid, 1);

    // We have a single filter, expect the backfill to be OSO
    runBackfill();

    // see comment in CollectionsOSODcpTest.basic
    consumer->snapshotMarker(
            1, replicaVB, 0, setup.second + 1, 0, 0, setup.second + 1);

    // Manually step the producer and inspect all callbacks
    EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));
    EXPECT_EQ(cb::mcbp::ClientOpcode::DcpOsoSnapshot, producers->last_op);
    EXPECT_EQ(uint32_t(cb::mcbp::request::DcpOsoSnapshotFlags::Start),
              producers->last_oso_snapshot_flags);

    EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));
    EXPECT_EQ(cb::mcbp::ClientOpcode::DcpSystemEvent, producers->last_op);
    EXPECT_EQ(CollectionUid::vegetable, producers->last_collection_id);
    EXPECT_EQ(mcbp::systemevent::id::DeleteCollection,
              producers->last_system_event);

    // ... end
    EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));
    EXPECT_EQ(cb::mcbp::ClientOpcode::DcpOsoSnapshot, producers->last_op);
    EXPECT_EQ(uint32_t(cb::mcbp::request::DcpOsoSnapshotFlags::End),
              producers->last_oso_snapshot_flags);
}

// Test that we can transition to in memory and continue.
TEST_F(CollectionsOSODcpTest, transition_to_memory) {
    // Write to default collection and deliberately not in lexicographical order
    store_item(vbid, makeStoredDocKey("b"), "b-value");
    store_item(vbid, makeStoredDocKey("a"), "a-value");
    store_item(vbid, makeStoredDocKey("c"), "c-value");
    flush_vbucket_to_disk(vbid, 3);

    // Reset so we have to stream from backfill
    resetEngineAndWarmup();

    createDcpObjects({{R"({"collections":["0"]})"}}, true /* enable oso */);

    // Some in-memory only item
    store_item(vbid, makeStoredDocKey("d"), "d-value");
    store_item(vbid, makeStoredDocKey("e"), "e-value");
    store_item(vbid, makeStoredDocKey("f"), "f-value");

    // We have a single filter, expect the backfill to be OSO
    runBackfill();

    // OSO snapshots are never really used in KV to KV replication, but this
    // test is using KV to KV test code, hence we need to set a snapshot so
    // that any transferred items don't trigger a snapshot exception.
    consumer->snapshotMarker(1, replicaVB, 0, 4, 0, 0, 4);

    // Manually step the producer and inspect all callbacks
    EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));
    EXPECT_EQ(cb::mcbp::ClientOpcode::DcpOsoSnapshot, producers->last_op);
    EXPECT_EQ(uint32_t(cb::mcbp::request::DcpOsoSnapshotFlags::Start),
              producers->last_oso_snapshot_flags);

    std::array<std::pair<std::string, uint64_t>, 3> keys = {
            {{"a", 2}, {"b", 1}, {"c", 3}}};
    for (const auto& [k, s] : keys) {
        EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));
        EXPECT_EQ(cb::mcbp::ClientOpcode::DcpMutation, producers->last_op);
        EXPECT_EQ(CollectionID::Default, producers->last_collection_id);
        EXPECT_EQ(k, producers->last_key);
        EXPECT_EQ(s, producers->last_byseqno);
    }

    // Now we get the end message
    EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));
    EXPECT_EQ(cb::mcbp::ClientOpcode::DcpOsoSnapshot, producers->last_op);
    EXPECT_EQ(uint32_t(cb::mcbp::request::DcpOsoSnapshotFlags::End),
              producers->last_oso_snapshot_flags);

    notifyAndStepToCheckpoint();

    EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));
    EXPECT_EQ(cb::mcbp::ClientOpcode::DcpMutation, producers->last_op);
    EXPECT_EQ("d", producers->last_key);
    EXPECT_EQ(4, producers->last_byseqno);
    EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));

    EXPECT_EQ(cb::mcbp::ClientOpcode::DcpMutation, producers->last_op);
    EXPECT_EQ("e", producers->last_key);
    EXPECT_EQ(5, producers->last_byseqno);
    EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));

    EXPECT_EQ(cb::mcbp::ClientOpcode::DcpMutation, producers->last_op);
    EXPECT_EQ("f", producers->last_key);
    EXPECT_EQ(6, producers->last_byseqno);
}

// Test that we can transition to in memory and continue (issue raised by
// MB-38999)
TEST_F(CollectionsOSODcpTest, transition_to_memory_MB_38999) {
    // Write to default collection and deliberately not in lexicographical order
    store_item(vbid, makeStoredDocKey("b"), "b-value");
    store_item(vbid, makeStoredDocKey("a"), "a-value");
    store_item(vbid, makeStoredDocKey("c"), "c-value");
    flush_vbucket_to_disk(vbid, 3);

    // Reset so we have to stream from backfill
    resetEngineAndWarmup();

    createDcpObjects({{R"({"collections":["0"]})"}}, true /* enable oso */);

    // Now write the 4th item and flush it
    store_item(vbid, makeStoredDocKey("d"), "d-value");
    flush_vbucket_to_disk(vbid, 1);

    // Now write the 5th item, not flushed
    store_item(vbid, makeStoredDocKey("e"), "e-value");

    // We have a single filter, expect the backfill to be OSO
    runBackfill();

    // OSO snapshots are never really used in KV to KV replication, but this
    // test is using KV to KV test code, hence we need to set a snapshot so
    // that any transferred items don't trigger a snapshot exception.
    consumer->snapshotMarker(1, replicaVB, 0, 4, 0, 0, 4);

    // Manually step the producer and inspect all callbacks
    EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));
    EXPECT_EQ(cb::mcbp::ClientOpcode::DcpOsoSnapshot, producers->last_op);
    EXPECT_EQ(uint32_t(cb::mcbp::request::DcpOsoSnapshotFlags::Start),
              producers->last_oso_snapshot_flags);

    std::array<std::pair<std::string, uint64_t>, 4> keys = {
            {{"a", 2}, {"b", 1}, {"c", 3}, {"d", 4}}};
    for (const auto& [k, s] : keys) {
        EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));
        EXPECT_EQ(cb::mcbp::ClientOpcode::DcpMutation, producers->last_op);
        EXPECT_EQ(CollectionID::Default, producers->last_collection_id);
        EXPECT_EQ(k, producers->last_key);
        EXPECT_EQ(s, producers->last_byseqno);
    }

    // Now we get the end message
    EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));
    EXPECT_EQ(cb::mcbp::ClientOpcode::DcpOsoSnapshot, producers->last_op);
    EXPECT_EQ(uint32_t(cb::mcbp::request::DcpOsoSnapshotFlags::End),
              producers->last_oso_snapshot_flags);

    notifyAndStepToCheckpoint();

    EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));
    EXPECT_EQ(cb::mcbp::ClientOpcode::DcpMutation, producers->last_op);
    EXPECT_EQ("e", producers->last_key);
    EXPECT_EQ(5, producers->last_byseqno);
}

// OSO doesn't support ephemeral - this one test checks it falls back to normal
// snapshots
class CollectionsOSOEphemeralTest : public CollectionsDcpParameterizedTest {
public:
    std::pair<CollectionsManifest, uint64_t> setupTwoCollections();
};

// Run through how we expect OSO to work, this is a minimal test which will
// use the default collection
TEST_P(CollectionsOSOEphemeralTest, basic) {
    // Write to default collection and deliberately not in lexicographical order
    store_item(vbid, makeStoredDocKey("b"), "q");
    store_item(vbid, makeStoredDocKey("d"), "a");
    store_item(vbid, makeStoredDocKey("a"), "w");
    store_item(vbid, makeStoredDocKey("c"), "y");

    ensureDcpWillBackfill();

    // Filter on default collection (this will request from seqno:0)
    createDcpObjects({{R"({"collections":["0"]})"}}, true /* enable oso */);

    runBackfill();

    // OSO snapshots are never really used in KV to KV replication, but this
    // test is using KV to KV test code, hence we need to set a snapshot so
    // that any transferred items don't trigger a snapshot exception.
    consumer->snapshotMarker(1, replicaVB, 0, 4, 0, 0, 4);

    // Manually step the producer and inspect all callbacks
    EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));
    EXPECT_EQ(cb::mcbp::ClientOpcode::DcpSnapshotMarker, producers->last_op);

    std::array<std::pair<std::string, uint64_t>, 4> keys = {
            {{"b", 1}, {"d", 2}, {"a", 3}, {"c", 4}}};
    for (const auto& [k, s] : keys) {
        EXPECT_EQ(ENGINE_SUCCESS, producer->stepWithBorderGuard(*producers));
        EXPECT_EQ(cb::mcbp::ClientOpcode::DcpMutation, producers->last_op);
        EXPECT_EQ(CollectionID::Default, producers->last_collection_id);
        EXPECT_EQ(k, producers->last_key);
        EXPECT_EQ(s, producers->last_byseqno);
    }
}

INSTANTIATE_TEST_SUITE_P(CollectionsOSOEphemeralTests,
                         CollectionsOSOEphemeralTest,
                         STParameterizedBucketTest::ephConfigValues(),
                         STParameterizedBucketTest::PrintToStringParamName);
