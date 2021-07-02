/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kFTDC

#include "mongo/platform/basic.h"

#include <list>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/is_master_response.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/network_interface.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/map_util.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(waitInHello);

using std::list;
using std::string;
using std::stringstream;
using std::unique_ptr;

namespace repl {
namespace {

constexpr auto kHelloString = "hello"_sd;
constexpr auto kCamelCaseIsMasterString = "isMaster"_sd;
constexpr auto kLowerCaseIsMasterString = "ismaster"_sd;

void appendReplicationInfo(OperationContext* opCtx,
                           BSONObjBuilder& result,
                           int level,
                           bool useLegacyResponseFields) {
    ReplicationCoordinator* replCoord = ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().usingReplSets()) {
        const auto& horizonParams = SplitHorizon::getParameters(opCtx->getClient());
        IsMasterResponse isMasterResponse;
        replCoord->fillIsMasterForReplSet(&isMasterResponse, horizonParams);
        result.appendElements(isMasterResponse.toBSON(useLegacyResponseFields));
        if (level) {
            replCoord->appendSlaveInfoData(&result);
        }
        return;
    }

    result.appendBool((useLegacyResponseFields ? "ismaster" : "isWritablePrimary"),
                      ReplicationCoordinator::get(opCtx)->isMasterForReportingPurposes());

    if (level) {
        BSONObjBuilder sources(result.subarrayStart("sources"));

        int n = 0;
        list<BSONObj> src;
        {
            const NamespaceString localSources{"local.sources"};
            AutoGetCollectionForReadCommand ctx(opCtx, localSources);
            auto exec = InternalPlanner::collectionScan(
                opCtx, localSources.ns(), ctx.getCollection(), PlanExecutor::NO_YIELD);
            BSONObj obj;
            PlanExecutor::ExecState state;
            while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL))) {
                src.push_back(obj.getOwned());
            }

            // Non-yielding collection scans from InternalPlanner will never error.
            invariant(PlanExecutor::IS_EOF == state);
        }

        for (list<BSONObj>::const_iterator i = src.begin(); i != src.end(); i++) {
            BSONObj s = *i;
            BSONObjBuilder bb;
            bb.append(s["host"]);
            string sourcename = s["source"].valuestr();
            if (sourcename != "main")
                bb.append(s["source"]);
            {
                BSONElement e = s["syncedTo"];
                BSONObjBuilder t(bb.subobjStart("syncedTo"));
                t.appendDate("time", e.timestampTime());
                t.append("inc", e.timestampInc());
                t.done();
            }

            if (level > 1) {
                invariant(!opCtx->lockState()->isLocked());
                // note: there is no so-style timeout on this connection; perhaps we should have
                // one.
                ScopedDbConnection conn(s["host"].valuestr());

                DBClientConnection* cliConn = dynamic_cast<DBClientConnection*>(&conn.conn());
                if (cliConn && replAuthenticate(cliConn)) {
                    BSONObj first = conn->findOne((string) "local.oplog.$" + sourcename,
                                                  Query().sort(BSON("$natural" << 1)));
                    BSONObj last = conn->findOne((string) "local.oplog.$" + sourcename,
                                                 Query().sort(BSON("$natural" << -1)));
                    bb.appendDate("masterFirst", first["ts"].timestampTime());
                    bb.appendDate("masterLast", last["ts"].timestampTime());
                    const auto lag = (last["ts"].timestampTime() - s["syncedTo"].timestampTime());
                    bb.append("lagSeconds", durationCount<Milliseconds>(lag) / 1000.0);
                }
                conn.done();
            }

            sources.append(BSONObjBuilder::numStr(n++), bb.obj());
        }

        sources.done();

        replCoord->appendSlaveInfoData(&result);
    }
}

class ReplicationInfoServerStatus : public ServerStatusSection {
public:
    ReplicationInfoServerStatus() : ServerStatusSection("repl") {}

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        if (!ReplicationCoordinator::get(opCtx)->isReplEnabled()) {
            return BSONObj();
        }

        int level = configElement.numberInt();

        BSONObjBuilder result;
        // TODO SERVER-50219: Change useLegacyResponseFields to false once the serverStatus changes
        // to remove master-slave terminology are merged.
        appendReplicationInfo(opCtx, result, level, true /* useLegacyResponseFields */);

        auto rbid = ReplicationProcess::get(opCtx)->getRollbackID();
        if (ReplicationProcess::kUninitializedRollbackId != rbid) {
            result.append("rbid", rbid);
        }

        return result.obj();
    }

} replicationInfoServerStatus;

class OplogInfoServerStatus : public ServerStatusSection {
public:
    OplogInfoServerStatus() : ServerStatusSection("oplog") {}

    bool includeByDefault() const override {
        return false;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        ReplicationCoordinator* replCoord = ReplicationCoordinator::get(opCtx);
        if (!replCoord->isReplEnabled()) {
            return BSONObj();
        }

        BSONObjBuilder result;
        // TODO(siyuan) Output term of OpTime
        result.append("latestOptime", replCoord->getMyLastAppliedOpTime().getTimestamp());

        BSONObj o;
        uassert(17347,
                "Problem reading earliest entry from oplog",
                Helpers::getSingleton(opCtx, NamespaceString::kRsOplogNamespace.ns().c_str(), o));
        result.append("earliestOptime", o["ts"].timestamp());
        return result.obj();
    }
} oplogInfoServerStatus;

// Fail point for Hello command. Returns sleep timeout if needed. Supported arguments:
//   internalClient:  enabled only for internal clients
//   notInternalClient: enabled only for non-internal clients
//   delay: specifies the sleep duration in milliseconds
//   uassert: pass this integer argument to uassert and throw
boost::optional<Milliseconds> handleHelloFailPoint(const BSONObj& args, const BSONObj& cmdObj) {
    if (args.hasElement("internalClient") && !cmdObj.hasElement("internalClient")) {
        log() << "Fail point Hello is disabled for external client";
        return boost::none;  // Filtered out not internal client.
    }
    if (args.hasElement("notInternalClient") && cmdObj.hasElement("internalClient")) {
        log() << "Fail point Hello is disabled for internal client";
        return boost::none;  // Filtered out internal client.
    }
    if (args.hasElement("delay")) {
        auto millisToSleep = args["delay"].numberInt();
        log() << "Fail point delays Hello response by " << millisToSleep << " ms";
        return Milliseconds(millisToSleep);
    }
    if (args.hasElement("uassert")) {
        log() << "Fail point fails Hello response";
        uasserted(args["uassert"].numberInt(), "Fail point");
    }
    return boost::none;
}

// Sleep implementation outside the fail point handler itself to avoid the problem that
// processing a fail point will block its state.
void sleepForDurationOrUntilShutdown(Milliseconds sleep) {
    while (sleep > Milliseconds(0) && !globalInShutdownDeprecated()) {
        auto nextSleep = std::min(sleep, Milliseconds(1000));
        try {
            sleepmillis(nextSleep.count());
            sleep -= nextSleep;
        } catch (...) {
            break;
        }
    }
}

class CmdHello : public BasicCommand {
public:
    CmdHello() : CmdHello(kHelloString, {}) {}

    bool requiresAuth() const final {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    std::string help() const override {
        return "Check if this server is primary for a replica set\n"
               "{ hello : 1 }";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const final {}  // No auth required

    bool run(OperationContext* opCtx,
             const string&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) final {
        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);

        boost::optional<Milliseconds> sleepTimeout;
        MONGO_FAIL_POINT_BLOCK(waitInHello, customArgs) {
            sleepTimeout = handleHelloFailPoint(customArgs.getData(), cmdObj);
        }
        if (MONGO_unlikely(sleepTimeout)) {
            sleepForDurationOrUntilShutdown(*sleepTimeout);
        }

        /* currently request to arbiter is (somewhat arbitrarily) an ismaster request that is not
           authenticated.
        */
        if (cmdObj["forShell"].trueValue()) {
            LastError::get(opCtx->getClient()).disable();
        }

        transport::Session::TagMask sessionTagsToSet = 0;
        transport::Session::TagMask sessionTagsToUnset = 0;

        // Tag connections to avoid closing them on stepdown.
        auto hangUpElement = cmdObj["hangUpOnStepDown"];
        if (!hangUpElement.eoo() && !hangUpElement.trueValue()) {
            sessionTagsToSet |= transport::Session::kKeepOpen;
        }

        auto client = opCtx->getClient();
        if (ClientMetadata::tryFinalize(client)) {
            // If we are the first hello, then set split horizon parameters.
            auto sniName = client->getSniNameForSession();
            SplitHorizon::setParameters(client, std::move(sniName));
        }

        // Parse the optional 'internalClient' field. This is provided by incoming connections from
        // mongod and mongos.
        auto internalClientElement = cmdObj["internalClient"];
        if (internalClientElement) {
            sessionTagsToSet |= transport::Session::kInternalClient;

            uassert(ErrorCodes::TypeMismatch,
                    str::stream() << "'internalClient' must be of type Object, but was of type "
                                  << typeName(internalClientElement.type()),
                    internalClientElement.type() == BSONType::Object);

            bool foundMaxWireVersion = false;
            for (auto&& elem : internalClientElement.Obj()) {
                auto fieldName = elem.fieldNameStringData();
                if (fieldName == "minWireVersion") {
                    // We do not currently use 'internalClient.minWireVersion'.
                    continue;
                } else if (fieldName == "maxWireVersion") {
                    foundMaxWireVersion = true;

                    uassert(ErrorCodes::TypeMismatch,
                            str::stream() << "'maxWireVersion' field of 'internalClient' must be "
                                             "of type int, but was of type "
                                          << typeName(elem.type()),
                            elem.type() == BSONType::NumberInt);

                    // All incoming connections from mongod/mongos of earlier versions should be
                    // closed if the featureCompatibilityVersion is bumped to 3.6.
                    if (elem.numberInt() >=
                        WireSpec::instance().incomingInternalClient.maxWireVersion) {
                        sessionTagsToSet |=
                            transport::Session::kLatestVersionInternalClientKeepOpen;
                    } else {
                        sessionTagsToUnset |=
                            transport::Session::kLatestVersionInternalClientKeepOpen;
                    }
                } else {
                    uasserted(ErrorCodes::BadValue,
                              str::stream() << "Unrecognized field of 'internalClient': '"
                                            << fieldName << "'");
                }
            }

            uassert(ErrorCodes::BadValue,
                    "Missing required field 'maxWireVersion' of 'internalClient'",
                    foundMaxWireVersion);
        } else {
            sessionTagsToUnset |= (transport::Session::kInternalClient |
                                   transport::Session::kLatestVersionInternalClientKeepOpen);
            sessionTagsToSet |= transport::Session::kExternalClientKeepOpen;
        }

        auto session = opCtx->getClient()->session();
        if (session) {
            session->mutateTags(
                [sessionTagsToSet, sessionTagsToUnset](transport::Session::TagMask originalTags) {
                    // After a mongos sends the initial "isMaster" command with its mongos client
                    // information, it sometimes sends another "isMaster" command that is forwarded
                    // from its client. Once kInternalClient has been set, we assume that any future
                    // "isMaster" commands are forwarded in this manner, and we do not update the
                    // session tags.
                    if ((originalTags & transport::Session::kInternalClient) == 0) {
                        return (originalTags | sessionTagsToSet) & ~sessionTagsToUnset;
                    } else {
                        return originalTags;
                    }
                });
        }

        appendReplicationInfo(opCtx, result, 0, useLegacyResponseFields());

        // Try to parse the optional 'helloOk' field. This should be provided on the initial
        // handshake for an incoming connection if the client supports the hello command. Clients
        // that specify 'helloOk' do not rely on "not master" error message parsing, which means
        // that we can safely return "not primary" error messages instead.
        bool helloOk = opCtx->getClient()->supportsHello();
        Status status = bsonExtractBooleanField(cmdObj, "helloOk", &helloOk);
        if (status.isOK()) {
            // If the hello request contains a "helloOk" field, set _supportsHello on the Client
            // to the value.
            opCtx->getClient()->setSupportsHello(helloOk);
            // Attach helloOk: true to the response so that the client knows the server supports
            // the hello command.
            result.append("helloOk", true);
        } else if (status.code() != ErrorCodes::NoSuchKey) {
            uassertStatusOK(status);
        }

        if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
            const int configServerModeNumber = 2;
            result.append("configsvr", configServerModeNumber);
        }

        result.appendNumber("maxBsonObjectSize", BSONObjMaxUserSize);
        result.appendNumber("maxMessageSizeBytes", MaxMessageSizeBytes);
        result.appendNumber("maxWriteBatchSize", write_ops::kMaxWriteBatchSize);
        result.appendDate("localTime", jsTime());
        result.append("logicalSessionTimeoutMinutes", localLogicalSessionTimeoutMinutes);
        result.appendNumber("connectionId", opCtx->getClient()->getConnectionId());

        if (internalClientElement) {
            result.append("minWireVersion",
                          WireSpec::instance().incomingInternalClient.minWireVersion);
            result.append("maxWireVersion",
                          WireSpec::instance().incomingInternalClient.maxWireVersion);
        } else {
            result.append("minWireVersion",
                          WireSpec::instance().incomingExternalClient.minWireVersion);
            result.append("maxWireVersion",
                          WireSpec::instance().incomingExternalClient.maxWireVersion);
        }

        result.append("readOnly", storageGlobalParams.readOnly);

        const auto parameter = mapFindWithDefault(ServerParameterSet::getGlobal()->getMap(),
                                                  "automationServiceDescriptor",
                                                  static_cast<ServerParameter*>(nullptr));
        if (parameter)
            parameter->append(opCtx, result, "automationServiceDescriptor");

        if (opCtx->getClient()->session()) {
            MessageCompressorManager::forSession(opCtx->getClient()->session())
                .serverNegotiate(cmdObj, &result);
        }

        auto& saslMechanismRegistry = SASLServerMechanismRegistry::get(opCtx->getServiceContext());
        saslMechanismRegistry.advertiseMechanismNamesForUser(opCtx, cmdObj, &result);

        return true;
    }

protected:
    CmdHello(const StringData cmdName, const std::initializer_list<StringData>& alias)
        : BasicCommand(cmdName, alias) {}

    virtual bool useLegacyResponseFields() {
        return false;
    }
} cmdhello;

class CmdIsMaster : public CmdHello {
public:
    CmdIsMaster() : CmdHello(kCamelCaseIsMasterString, {kLowerCaseIsMasterString}) {}

    std::string help() const override {
        return "Check if this server is primary for a replica set\n"
               "{ isMaster : 1 }";
    }

protected:
    // Parse the command name, which should be one of the following: hello, isMaster, or
    // ismaster. If the command is "hello", we must attach an "isWritablePrimary" response field
    // instead of "ismaster" and "secondaryDelaySecs" response field instead of "slaveDelay".
    bool useLegacyResponseFields() override {
        return true;
    }

} cmdIsMaster;

OpCounterServerStatusSection replOpCounterServerStatusSection("opcountersRepl", &replOpCounters);

}  // namespace

}  // namespace repl
}  // namespace mongo
