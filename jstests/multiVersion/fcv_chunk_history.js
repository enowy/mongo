/**
 * Tests that chunk histories are properly generated and stored when upgrading from FCV 3.6 to FCV
 * 4.0 in a sharded cluster.
 */

(function() {
    "use strict";

    load("jstests/libs/feature_compatibility_version.js");

    var st = new ShardingTest({
        shards: 1,
    });

    let configPrimary = st.configRS.getPrimary();
    let configPrimaryAdminDB = configPrimary.getDB("admin");
    let shardPrimary = st.rs0.getPrimary();
    let shardPrimaryAdminDB = shardPrimary.getDB("admin");
    let shardPrimaryConfigDB = shardPrimary.getDB("config");

    // Change FCV to last stable so chunks will not have histories.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "3.6"}));
    checkFCV(configPrimaryAdminDB, "3.6");
    checkFCV(shardPrimaryAdminDB, "3.6");

    let testDB = st.s.getDB("test1");

    // Create a sharded collection with primary shard 0.
    assert.commandWorked(st.s.adminCommand({enableSharding: testDB.getName()}));
    st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);
    assert.commandWorked(
        st.s.adminCommand({shardCollection: testDB.foo.getFullName(), key: {a: 1}}));
    assert.commandWorked(st.s.adminCommand({split: testDB.foo.getFullName(), middle: {a: 0}}));
    assert.commandWorked(st.s.adminCommand({split: testDB.foo.getFullName(), middle: {a: -1000}}));
    assert.commandWorked(st.s.adminCommand({split: testDB.foo.getFullName(), middle: {a: +1000}}));

    assert.writeOK(st.s.getDB("test1").foo.insert({_id: "id1", a: 1}));
    assert.neq(null, st.s.getDB("test1").foo.findOne({_id: "id1", a: 1}));

    assert.writeOK(st.s.getDB("test1").foo.insert({_id: "id2", a: -1}));
    assert.neq(null, st.s.getDB("test1").foo.findOne({_id: "id2", a: -1}));

    // Make sure chunks do not have history when FCV is 3.6.
    let chunks = st.s.getDB("config").getCollection("chunks").find({ns: "test1.foo"}).toArray();
    assert.eq(chunks.length, 4);
    chunks.forEach((chunk) => {
        assert.neq(null, chunk);
        assert(!chunk.hasOwnProperty("history"), "test1.foo has a history before upgrade");
        assert(!chunk.hasOwnProperty("historyIsAt40"),
               "test1.foo has a historyIsAt40 before upgrade");
    });
    chunks = shardPrimaryConfigDB.getCollection("cache.chunks.test1.foo").find().toArray();
    assert.eq(chunks.length, 4);
    chunks.forEach((chunk) => {
        assert.neq(null, chunk);
        assert(!chunk.hasOwnProperty("history"),
               "test1.foo has a history on the shard before upgrade");
    });

    // Set FCV to 4.0.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "4.0"}));
    checkFCV(configPrimaryAdminDB, latestFCV);
    checkFCV(shardPrimaryAdminDB, latestFCV);

    // Make sure chunks for test1.foo were given history after upgrade.
    chunks = st.s.getDB("config").getCollection("chunks").find({ns: "test1.foo"}).toArray();
    assert.eq(chunks.length, 4);
    chunks.forEach((chunk) => {
        assert.neq(null, chunk);
        assert(chunk.hasOwnProperty("history"), "test1.foo does not have a history after upgrade");
        assert(chunk.hasOwnProperty("historyIsAt40"),
               "test1.foo does not have a historyIsAt40 after upgrade");
    });
    chunks = shardPrimaryConfigDB.getCollection("cache.chunks.test1.foo").find().toArray();
    assert.eq(chunks.length, 4);
    chunks.forEach((chunk) => {
        assert.neq(null, chunk);
        assert(chunk.hasOwnProperty("history"),
               "test1.foo does not have a history on the shard after upgrade");
    });

    // Set FCV to 3.6.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "3.6"}));
    checkFCV(configPrimaryAdminDB, "3.6");
    checkFCV(shardPrimaryAdminDB, "3.6");

    // Make sure history was removed from the config server entries when FCV changed to 3.6.
    chunks = st.s.getDB("config").getCollection("chunks").find({ns: "test1.foo"}).toArray();
    assert.eq(chunks.length, 4);
    chunks.forEach((chunk) => {
        assert.neq(null, chunk);
        assert(!chunk.hasOwnProperty("history"), "test1.foo has a history after downgrade");
        assert(!chunk.hasOwnProperty("historyIsAt40"),
               "test1.foo has a historyIsAt40 after downgrade");
    });

    // Set FCV back to 4.0 in order to test partially-upgraded chunk histories due to SERVER-62065
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "4.0"}));
    checkFCV(configPrimaryAdminDB, latestFCV);
    checkFCV(shardPrimaryAdminDB, latestFCV);

    // Manually clear the 'historyIsAt40' field from the config server and the history entries from
    // the shards' cache collections in order to simulate a wrong upgrade due to SERVER-62065
    assert.writeOK(st.s.getDB("config").chunks.update(
        {ns: 'test1.foo'}, {'$unset': {historyIsAt40: ''}}, {multi: true}));
    assert.writeOK(shardPrimaryConfigDB.getCollection("cache.chunks.test1.foo")
                       .update({}, {'$unset': {history: ''}}, {multi: true}));

    assert.commandWorked(st.s.adminCommand({repairShardedCollectionChunksHistory: 'test1.foo'}));

    // Make sure chunks for test1.foo were given history after repair.
    chunks = st.s.getDB("config").getCollection("chunks").find({ns: "test1.foo"}).toArray();
    assert.eq(chunks.length, 4);
    chunks.forEach((chunk) => {
        assert.neq(null, chunk);
        assert(chunk.hasOwnProperty("history"), "test1.foo does not have a history after repair");
        assert(chunk.hasOwnProperty("historyIsAt40"),
               "test1.foo does not have a historyIsAt40 after repair");
    });
    chunks = shardPrimaryConfigDB.getCollection("cache.chunks.test1.foo").find().toArray();
    assert.eq(chunks.length, 4);
    chunks.forEach((chunk) => {
        assert.neq(null, chunk);
        assert(chunk.hasOwnProperty("history"),
               "test1.foo does not have a history on the shard after repair");
    });

    st.stop();
})();
