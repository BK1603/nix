#include "serialise.hh"
#include "util.hh"
#include "remote-fs-accessor.hh"
#include "remote-store.hh"
#include "worker-protocol.hh"
#include "archive.hh"
#include "affinity.hh"
#include "globals.hh"
#include "derivations.hh"
#include "pool.hh"
#include "finally.hh"
#include "logging.hh"
#include "callback.hh"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>

namespace nix {


template<> StorePathSet readStorePaths(const Store & store, Source & from)
{
    StorePathSet paths;
    for (auto & i : readStrings<Strings>(from))
        paths.insert(store.parseStorePath(i));
    return paths;
}

void writeStorePaths(const Store & store, Sink & out, const StorePathSet & paths)
{
    out << paths.size();
    for (auto & i : paths)
        out << store.printStorePath(i);
}


StorePathCAMap readStorePathCAMap(const Store & store, Source & from)
{
    StorePathCAMap paths;
    auto count = readNum<size_t>(from);
    while (count--) {
        auto path = store.parseStorePath(readString(from));
        auto ca = parseContentAddressOpt(readString(from));
        paths.insert_or_assign(path, ca);
    }
    return paths;
}

void writeStorePathCAMap(const Store & store, Sink & out, const StorePathCAMap & paths)
{
    out << paths.size();
    for (auto & i : paths) {
        out << store.printStorePath(i.first);
        out << renderContentAddress(i.second);
    }
}


namespace worker_proto {

StorePath read(const Store & store, Source & from, Phantom<StorePath> _)
{
    return store.parseStorePath(readString(from));
}

void write(const Store & store, Sink & out, const StorePath & storePath)
{
    out << store.printStorePath(storePath);
}


template<>
std::optional<StorePath> read(const Store & store, Source & from, Phantom<std::optional<StorePath>> _)
{
    auto s = readString(from);
    return s == "" ? std::optional<StorePath> {} : store.parseStorePath(s);
}

template<>
void write(const Store & store, Sink & out, const std::optional<StorePath> & storePathOpt)
{
    out << (storePathOpt ? store.printStorePath(*storePathOpt) : "");
}

}


/* TODO: Separate these store impls into different files, give them better names */
RemoteStore::RemoteStore(const Params & params)
    : Store(params)
    , RemoteStoreConfig(params)
    , connections(make_ref<Pool<Connection>>(
            std::max(1, (int) maxConnections),
            [this]() {
                auto conn = openConnectionWrapper();
                try {
                    initConnection(*conn);
                } catch (...) {
                    failed = true;
                    throw;
                }
                return conn;
            },
            [this](const ref<Connection> & r) {
                return
                    r->to.good()
                    && r->from.good()
                    && std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - r->startTime).count() < maxConnectionAge;
            }
            ))
{
}


ref<RemoteStore::Connection> RemoteStore::openConnectionWrapper()
{
    if (failed)
        throw Error("opening a connection to remote store '%s' previously failed", getUri());
    try {
        return openConnection();
    } catch (...) {
        failed = true;
        throw;
    }
}


UDSRemoteStore::UDSRemoteStore(const Params & params)
    : StoreConfig(params)
    , Store(params)
    , LocalFSStore(params)
    , RemoteStore(params)
{
}


UDSRemoteStore::UDSRemoteStore(
        const std::string scheme,
        std::string socket_path,
        const Params & params)
    : UDSRemoteStore(params)
{
    path.emplace(socket_path);
}


std::string UDSRemoteStore::getUri()
{
    if (path) {
        return std::string("unix://") + *path;
    } else {
        return "daemon";
    }
}


ref<RemoteStore::Connection> UDSRemoteStore::openConnection()
{
    auto conn = make_ref<Connection>();

    /* Connect to a daemon that does the privileged work for us. */
    conn->fd = socket(PF_UNIX, SOCK_STREAM
        #ifdef SOCK_CLOEXEC
        | SOCK_CLOEXEC
        #endif
        , 0);
    if (!conn->fd)
        throw SysError("cannot create Unix domain socket");
    closeOnExec(conn->fd.get());

    string socketPath = path ? *path : settings.nixDaemonSocketFile;

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    if (socketPath.size() + 1 >= sizeof(addr.sun_path))
        throw Error("socket path '%1%' is too long", socketPath);
    strcpy(addr.sun_path, socketPath.c_str());

    if (::connect(conn->fd.get(), (struct sockaddr *) &addr, sizeof(addr)) == -1)
        throw SysError("cannot connect to daemon at '%1%'", socketPath);

    conn->from.fd = conn->fd.get();
    conn->to.fd = conn->fd.get();

    conn->startTime = std::chrono::steady_clock::now();

    return conn;
}


void RemoteStore::initConnection(Connection & conn)
{
    /* Send the magic greeting, check for the reply. */
    try {
        conn.to << WORKER_MAGIC_1;
        conn.to.flush();
        unsigned int magic = readInt(conn.from);
        if (magic != WORKER_MAGIC_2) throw Error("protocol mismatch");

        conn.from >> conn.daemonVersion;
        if (GET_PROTOCOL_MAJOR(conn.daemonVersion) != GET_PROTOCOL_MAJOR(PROTOCOL_VERSION))
            throw Error("Nix daemon protocol version not supported");
        if (GET_PROTOCOL_MINOR(conn.daemonVersion) < 10)
            throw Error("the Nix daemon version is too old");
        conn.to << PROTOCOL_VERSION;

        if (GET_PROTOCOL_MINOR(conn.daemonVersion) >= 14) {
            int cpu = sameMachine() && settings.lockCPU ? lockToCurrentCPU() : -1;
            if (cpu != -1)
                conn.to << 1 << cpu;
            else
                conn.to << 0;
        }

        if (GET_PROTOCOL_MINOR(conn.daemonVersion) >= 11)
            conn.to << false;

        auto ex = conn.processStderr();
        if (ex) std::rethrow_exception(ex);
    }
    catch (Error & e) {
        throw Error("cannot open connection to remote store '%s': %s", getUri(), e.what());
    }

    setOptions(conn);
}


void RemoteStore::setOptions(Connection & conn)
{
    conn.to << wopSetOptions
       << settings.keepFailed
       << settings.keepGoing
       << settings.tryFallback
       << verbosity
       << settings.maxBuildJobs
       << settings.maxSilentTime
       << true
       << (settings.verboseBuild ? lvlError : lvlVomit)
       << 0 // obsolete log type
       << 0 /* obsolete print build trace */
       << settings.buildCores
       << settings.useSubstitutes;

    if (GET_PROTOCOL_MINOR(conn.daemonVersion) >= 12) {
        std::map<std::string, Config::SettingInfo> overrides;
        globalConfig.getSettings(overrides, true);
        overrides.erase(settings.keepFailed.name);
        overrides.erase(settings.keepGoing.name);
        overrides.erase(settings.tryFallback.name);
        overrides.erase(settings.maxBuildJobs.name);
        overrides.erase(settings.maxSilentTime.name);
        overrides.erase(settings.buildCores.name);
        overrides.erase(settings.useSubstitutes.name);
        overrides.erase(loggerSettings.showTrace.name);
        conn.to << overrides.size();
        for (auto & i : overrides)
            conn.to << i.first << i.second.value;
    }

    auto ex = conn.processStderr();
    if (ex) std::rethrow_exception(ex);
}


/* A wrapper around Pool<RemoteStore::Connection>::Handle that marks
   the connection as bad (causing it to be closed) if a non-daemon
   exception is thrown before the handle is closed. Such an exception
   causes a deviation from the expected protocol and therefore a
   desynchronization between the client and daemon. */
struct ConnectionHandle
{
    Pool<RemoteStore::Connection>::Handle handle;
    bool daemonException = false;

    ConnectionHandle(Pool<RemoteStore::Connection>::Handle && handle)
        : handle(std::move(handle))
    { }

    ConnectionHandle(ConnectionHandle && h)
        : handle(std::move(h.handle))
    { }

    ~ConnectionHandle()
    {
        if (!daemonException && std::uncaught_exceptions()) {
            handle.markBad();
            debug("closing daemon connection because of an exception");
        }
    }

    RemoteStore::Connection * operator -> () { return &*handle; }

    void processStderr(Sink * sink = 0, Source * source = 0, bool flush = true)
    {
        auto ex = handle->processStderr(sink, source, flush);
        if (ex) {
            daemonException = true;
            std::rethrow_exception(ex);
        }
    }

    void withFramedSink(std::function<void(Sink & sink)> fun);
};


ConnectionHandle RemoteStore::getConnection()
{
    return ConnectionHandle(connections->get());
}


bool RemoteStore::isValidPathUncached(const StorePath & path)
{
    auto conn(getConnection());
    conn->to << wopIsValidPath << printStorePath(path);
    conn.processStderr();
    return readInt(conn->from);
}


StorePathSet RemoteStore::queryValidPaths(const StorePathSet & paths, SubstituteFlag maybeSubstitute)
{
    auto conn(getConnection());
    if (GET_PROTOCOL_MINOR(conn->daemonVersion) < 12) {
        StorePathSet res;
        for (auto & i : paths)
            if (isValidPath(i)) res.insert(i);
        return res;
    } else {
        conn->to << wopQueryValidPaths;
        writeStorePaths(*this, conn->to, paths);
        conn.processStderr();
        return readStorePaths<StorePathSet>(*this, conn->from);
    }
}


StorePathSet RemoteStore::queryAllValidPaths()
{
    auto conn(getConnection());
    conn->to << wopQueryAllValidPaths;
    conn.processStderr();
    return readStorePaths<StorePathSet>(*this, conn->from);
}


StorePathSet RemoteStore::querySubstitutablePaths(const StorePathSet & paths)
{
    auto conn(getConnection());
    if (GET_PROTOCOL_MINOR(conn->daemonVersion) < 12) {
        StorePathSet res;
        for (auto & i : paths) {
            conn->to << wopHasSubstitutes << printStorePath(i);
            conn.processStderr();
            if (readInt(conn->from)) res.insert(i);
        }
        return res;
    } else {
        conn->to << wopQuerySubstitutablePaths;
        writeStorePaths(*this, conn->to, paths);
        conn.processStderr();
        return readStorePaths<StorePathSet>(*this, conn->from);
    }
}


void RemoteStore::querySubstitutablePathInfos(const StorePathCAMap & pathsMap, SubstitutablePathInfos & infos)
{
    if (pathsMap.empty()) return;

    auto conn(getConnection());

    if (GET_PROTOCOL_MINOR(conn->daemonVersion) < 12) {

        for (auto & i : pathsMap) {
            SubstitutablePathInfo info;
            conn->to << wopQuerySubstitutablePathInfo << printStorePath(i.first);
            conn.processStderr();
            unsigned int reply = readInt(conn->from);
            if (reply == 0) continue;
            auto deriver = readString(conn->from);
            if (deriver != "")
                info.deriver = parseStorePath(deriver);
            info.references = readStorePaths<StorePathSet>(*this, conn->from);
            info.downloadSize = readLongLong(conn->from);
            info.narSize = readLongLong(conn->from);
            infos.insert_or_assign(i.first, std::move(info));
        }

    } else {

        conn->to << wopQuerySubstitutablePathInfos;
        if (GET_PROTOCOL_MINOR(conn->daemonVersion) < 22) {
            StorePathSet paths;
            for (auto & path : pathsMap)
                paths.insert(path.first);
            writeStorePaths(*this, conn->to, paths);
        } else
            writeStorePathCAMap(*this, conn->to, pathsMap);
        conn.processStderr();
        size_t count = readNum<size_t>(conn->from);
        for (size_t n = 0; n < count; n++) {
            SubstitutablePathInfo & info(infos[parseStorePath(readString(conn->from))]);
            auto deriver = readString(conn->from);
            if (deriver != "")
                info.deriver = parseStorePath(deriver);
            info.references = readStorePaths<StorePathSet>(*this, conn->from);
            info.downloadSize = readLongLong(conn->from);
            info.narSize = readLongLong(conn->from);
        }

    }
}


ref<const ValidPathInfo> RemoteStore::readValidPathInfo(ConnectionHandle & conn, const StorePath & path)
{
    auto deriver = readString(conn->from);
    auto narHash = Hash::parseAny(readString(conn->from), htSHA256);
    auto info = make_ref<ValidPathInfo>(path, narHash);
    if (deriver != "") info->deriver = parseStorePath(deriver);
    info->references = readStorePaths<StorePathSet>(*this, conn->from);
    conn->from >> info->registrationTime >> info->narSize;
    if (GET_PROTOCOL_MINOR(conn->daemonVersion) >= 16) {
        conn->from >> info->ultimate;
        info->sigs = readStrings<StringSet>(conn->from);
        info->ca = parseContentAddressOpt(readString(conn->from));
    }
    return info;
}


void RemoteStore::queryPathInfoUncached(const StorePath & path,
    Callback<std::shared_ptr<const ValidPathInfo>> callback) noexcept
{
    try {
        std::shared_ptr<const ValidPathInfo> info;
        {
            auto conn(getConnection());
            conn->to << wopQueryPathInfo << printStorePath(path);
            try {
                conn.processStderr();
            } catch (Error & e) {
                // Ugly backwards compatibility hack.
                if (e.msg().find("is not valid") != std::string::npos)
                    throw InvalidPath(e.info());
                throw;
            }
            if (GET_PROTOCOL_MINOR(conn->daemonVersion) >= 17) {
                bool valid; conn->from >> valid;
                if (!valid) throw InvalidPath("path '%s' is not valid", printStorePath(path));
            }
            info = readValidPathInfo(conn, path);
        }
        callback(std::move(info));
    } catch (...) { callback.rethrow(); }
}


void RemoteStore::queryReferrers(const StorePath & path,
    StorePathSet & referrers)
{
    auto conn(getConnection());
    conn->to << wopQueryReferrers << printStorePath(path);
    conn.processStderr();
    for (auto & i : readStorePaths<StorePathSet>(*this, conn->from))
        referrers.insert(i);
}


StorePathSet RemoteStore::queryValidDerivers(const StorePath & path)
{
    auto conn(getConnection());
    conn->to << wopQueryValidDerivers << printStorePath(path);
    conn.processStderr();
    return readStorePaths<StorePathSet>(*this, conn->from);
}


StorePathSet RemoteStore::queryDerivationOutputs(const StorePath & path)
{
    auto conn(getConnection());
    if (GET_PROTOCOL_MINOR(conn->daemonVersion) >= 0x16) {
        return Store::queryDerivationOutputs(path);
    }
    conn->to << wopQueryDerivationOutputs << printStorePath(path);
    conn.processStderr();
    return readStorePaths<StorePathSet>(*this, conn->from);
}


std::map<std::string, std::optional<StorePath>> RemoteStore::queryPartialDerivationOutputMap(const StorePath & path)
{
    if (GET_PROTOCOL_MINOR(getProtocol()) >= 0x16) {
        auto conn(getConnection());
        conn->to << wopQueryDerivationOutputMap << printStorePath(path);
        conn.processStderr();
        return worker_proto::read(*this, conn->from, Phantom<std::map<std::string, std::optional<StorePath>>> {});
    } else {
        // Fallback for old daemon versions.
        // For floating-CA derivations (and their co-dependencies) this is an
        // under-approximation as it only returns the paths that can be inferred
        // from the derivation itself (and not the ones that are known because
        // the have been built), but as old stores don't handle floating-CA
        // derivations this shouldn't matter
        auto derivation = readDerivation(path);
        auto outputsWithOptPaths = derivation.outputsAndOptPaths(*this);
        std::map<std::string, std::optional<StorePath>> ret;
        for (auto & [outputName, outputAndPath] : outputsWithOptPaths) {
            ret.emplace(outputName, outputAndPath.second);
        }
        return ret;
    }

}

std::optional<StorePath> RemoteStore::queryPathFromHashPart(const std::string & hashPart)
{
    auto conn(getConnection());
    conn->to << wopQueryPathFromHashPart << hashPart;
    conn.processStderr();
    Path path = readString(conn->from);
    if (path.empty()) return {};
    return parseStorePath(path);
}


ref<const ValidPathInfo> RemoteStore::addCAToStore(
    Source & dump,
    const string & name,
    ContentAddressMethod caMethod,
    const StorePathSet & references,
    RepairFlag repair)
{
    std::optional<ConnectionHandle> conn_(getConnection());
    auto & conn = *conn_;

    if (GET_PROTOCOL_MINOR(conn->daemonVersion) >= 25) {

        conn->to
            << wopAddToStore
            << name
            << renderContentAddressMethod(caMethod);
        writeStorePaths(*this, conn->to, references);
        conn->to << repair;

        conn.withFramedSink([&](Sink & sink) {
            dump.drainInto(sink);
        });

        auto path = parseStorePath(readString(conn->from));
        return readValidPathInfo(conn, path);
    }
    else {
        if (repair) throw Error("repairing is not supported when building through the Nix daemon protocol < 1.25");

        std::visit(overloaded {
            [&](TextHashMethod thm) -> void {
                std::string s = dump.drain();
                conn->to << wopAddTextToStore << name << s;
                writeStorePaths(*this, conn->to, references);
                conn.processStderr();
            },
            [&](FixedOutputHashMethod fohm) -> void {
                conn->to
                    << wopAddToStore
                    << name
                    << ((fohm.hashType == htSHA256 && fohm.fileIngestionMethod == FileIngestionMethod::Recursive) ? 0 : 1) /* backwards compatibility hack */
                    << (fohm.fileIngestionMethod == FileIngestionMethod::Recursive ? 1 : 0)
                    << printHashType(fohm.hashType);

                try {
                    conn->to.written = 0;
                    conn->to.warn = true;
                    connections->incCapacity();
                    {
                        Finally cleanup([&]() { connections->decCapacity(); });
                        if (fohm.fileIngestionMethod == FileIngestionMethod::Recursive) {
                            dump.drainInto(conn->to);
                        } else {
                            std::string contents = dump.drain();
                            dumpString(contents, conn->to);
                        }
                    }
                    conn->to.warn = false;
                    conn.processStderr();
                } catch (SysError & e) {
                    /* Daemon closed while we were sending the path. Probably OOM
                      or I/O error. */
                    if (e.errNo == EPIPE)
                        try {
                            conn.processStderr();
                        } catch (EndOfFile & e) { }
                    throw;
                }

            }
        }, caMethod);
        auto path = parseStorePath(readString(conn->from));
        // Release our connection to prevent a deadlock in queryPathInfo().
        conn_.reset();
        return queryPathInfo(path);
    }
}


StorePath RemoteStore::addToStoreFromDump(Source & dump, const string & name,
        FileIngestionMethod method, HashType hashType, RepairFlag repair)
{
    StorePathSet references;
    return addCAToStore(dump, name, FixedOutputHashMethod{ .fileIngestionMethod = method, .hashType = hashType }, references, repair)->path;
}


void RemoteStore::addToStore(const ValidPathInfo & info, Source & source,
    RepairFlag repair, CheckSigsFlag checkSigs)
{
    auto conn(getConnection());

    if (GET_PROTOCOL_MINOR(conn->daemonVersion) < 18) {
        conn->to << wopImportPaths;

        auto source2 = sinkToSource([&](Sink & sink) {
            sink << 1 // == path follows
                ;
            copyNAR(source, sink);
            sink
                << exportMagic
                << printStorePath(info.path);
            writeStorePaths(*this, sink, info.references);
            sink
                << (info.deriver ? printStorePath(*info.deriver) : "")
                << 0 // == no legacy signature
                << 0 // == no path follows
                ;
        });

        conn.processStderr(0, source2.get());

        auto importedPaths = readStorePaths<StorePathSet>(*this, conn->from);
        assert(importedPaths.size() <= 1);
    }

    else {
        conn->to << wopAddToStoreNar
                 << printStorePath(info.path)
                 << (info.deriver ? printStorePath(*info.deriver) : "")
                 << info.narHash.to_string(Base16, false);
        writeStorePaths(*this, conn->to, info.references);
        conn->to << info.registrationTime << info.narSize
                 << info.ultimate << info.sigs << renderContentAddress(info.ca)
                 << repair << !checkSigs;

        if (GET_PROTOCOL_MINOR(conn->daemonVersion) >= 23) {
            conn.withFramedSink([&](Sink & sink) {
                copyNAR(source, sink);
            });
        } else if (GET_PROTOCOL_MINOR(conn->daemonVersion) >= 21) {
            conn.processStderr(0, &source);
        } else {
            copyNAR(source, conn->to);
            conn.processStderr(0, nullptr);
        }
    }
}


StorePath RemoteStore::addTextToStore(const string & name, const string & s,
    const StorePathSet & references, RepairFlag repair)
{
    StringSource source(s);
    return addCAToStore(source, name, TextHashMethod{}, references, repair)->path;
}


void RemoteStore::buildPaths(const std::vector<StorePathWithOutputs> & drvPaths, BuildMode buildMode)
{
    auto conn(getConnection());
    conn->to << wopBuildPaths;
    assert(GET_PROTOCOL_MINOR(conn->daemonVersion) >= 13);
    Strings ss;
    for (auto & p : drvPaths)
        ss.push_back(p.to_string(*this));
    conn->to << ss;
    if (GET_PROTOCOL_MINOR(conn->daemonVersion) >= 15)
        conn->to << buildMode;
    else
        /* Old daemons did not take a 'buildMode' parameter, so we
           need to validate it here on the client side.  */
        if (buildMode != bmNormal)
            throw Error("repairing or checking is not supported when building through the Nix daemon");
    conn.processStderr();
    readInt(conn->from);
}


BuildResult RemoteStore::buildDerivation(const StorePath & drvPath, const BasicDerivation & drv,
    BuildMode buildMode)
{
    auto conn(getConnection());
    conn->to << wopBuildDerivation << printStorePath(drvPath);
    writeDerivation(conn->to, *this, drv);
    conn->to << buildMode;
    conn.processStderr();
    BuildResult res;
    unsigned int status;
    conn->from >> status >> res.errorMsg;
    res.status = (BuildResult::Status) status;
    return res;
}


void RemoteStore::ensurePath(const StorePath & path)
{
    auto conn(getConnection());
    conn->to << wopEnsurePath << printStorePath(path);
    conn.processStderr();
    readInt(conn->from);
}


void RemoteStore::addTempRoot(const StorePath & path)
{
    auto conn(getConnection());
    conn->to << wopAddTempRoot << printStorePath(path);
    conn.processStderr();
    readInt(conn->from);
}


void RemoteStore::addIndirectRoot(const Path & path)
{
    auto conn(getConnection());
    conn->to << wopAddIndirectRoot << path;
    conn.processStderr();
    readInt(conn->from);
}


void RemoteStore::syncWithGC()
{
    auto conn(getConnection());
    conn->to << wopSyncWithGC;
    conn.processStderr();
    readInt(conn->from);
}


Roots RemoteStore::findRoots(bool censor)
{
    auto conn(getConnection());
    conn->to << wopFindRoots;
    conn.processStderr();
    size_t count = readNum<size_t>(conn->from);
    Roots result;
    while (count--) {
        Path link = readString(conn->from);
        auto target = parseStorePath(readString(conn->from));
        result[std::move(target)].emplace(link);
    }
    return result;
}


void RemoteStore::collectGarbage(const GCOptions & options, GCResults & results)
{
    auto conn(getConnection());

    conn->to
        << wopCollectGarbage << options.action;
    writeStorePaths(*this, conn->to, options.pathsToDelete);
    conn->to << options.ignoreLiveness
        << options.maxFreed
        /* removed options */
        << 0 << 0 << 0;

    conn.processStderr();

    results.paths = readStrings<PathSet>(conn->from);
    results.bytesFreed = readLongLong(conn->from);
    readLongLong(conn->from); // obsolete

    {
        auto state_(Store::state.lock());
        state_->pathInfoCache.clear();
    }
}


void RemoteStore::optimiseStore()
{
    auto conn(getConnection());
    conn->to << wopOptimiseStore;
    conn.processStderr();
    readInt(conn->from);
}


bool RemoteStore::verifyStore(bool checkContents, RepairFlag repair)
{
    auto conn(getConnection());
    conn->to << wopVerifyStore << checkContents << repair;
    conn.processStderr();
    return readInt(conn->from);
}


void RemoteStore::addSignatures(const StorePath & storePath, const StringSet & sigs)
{
    auto conn(getConnection());
    conn->to << wopAddSignatures << printStorePath(storePath) << sigs;
    conn.processStderr();
    readInt(conn->from);
}


void RemoteStore::queryMissing(const std::vector<StorePathWithOutputs> & targets,
    StorePathSet & willBuild, StorePathSet & willSubstitute, StorePathSet & unknown,
    uint64_t & downloadSize, uint64_t & narSize)
{
    {
        auto conn(getConnection());
        if (GET_PROTOCOL_MINOR(conn->daemonVersion) < 19)
            // Don't hold the connection handle in the fallback case
            // to prevent a deadlock.
            goto fallback;
        conn->to << wopQueryMissing;
        Strings ss;
        for (auto & p : targets)
            ss.push_back(p.to_string(*this));
        conn->to << ss;
        conn.processStderr();
        willBuild = readStorePaths<StorePathSet>(*this, conn->from);
        willSubstitute = readStorePaths<StorePathSet>(*this, conn->from);
        unknown = readStorePaths<StorePathSet>(*this, conn->from);
        conn->from >> downloadSize >> narSize;
        return;
    }

 fallback:
    return Store::queryMissing(targets, willBuild, willSubstitute,
        unknown, downloadSize, narSize);
}


void RemoteStore::connect()
{
    auto conn(getConnection());
}


unsigned int RemoteStore::getProtocol()
{
    auto conn(connections->get());
    return conn->daemonVersion;
}


void RemoteStore::flushBadConnections()
{
    connections->flushBad();
}


RemoteStore::Connection::~Connection()
{
    try {
        to.flush();
    } catch (...) {
        ignoreException();
    }
}

void RemoteStore::narFromPath(const StorePath & path, Sink & sink)
{
    auto conn(connections->get());
    conn->to << wopNarFromPath << printStorePath(path);
    conn->processStderr();
    copyNAR(conn->from, sink);
}

ref<FSAccessor> RemoteStore::getFSAccessor()
{
    return make_ref<RemoteFSAccessor>(ref<Store>(shared_from_this()));
}

static Logger::Fields readFields(Source & from)
{
    Logger::Fields fields;
    size_t size = readInt(from);
    for (size_t n = 0; n < size; n++) {
        auto type = (decltype(Logger::Field::type)) readInt(from);
        if (type == Logger::Field::tInt)
            fields.push_back(readNum<uint64_t>(from));
        else if (type == Logger::Field::tString)
            fields.push_back(readString(from));
        else
            throw Error("got unsupported field type %x from Nix daemon", (int) type);
    }
    return fields;
}


std::exception_ptr RemoteStore::Connection::processStderr(Sink * sink, Source * source, bool flush)
{
    if (flush)
        to.flush();

    while (true) {

        auto msg = readNum<uint64_t>(from);

        if (msg == STDERR_WRITE) {
            string s = readString(from);
            if (!sink) throw Error("no sink");
            (*sink)(s);
        }

        else if (msg == STDERR_READ) {
            if (!source) throw Error("no source");
            size_t len = readNum<size_t>(from);
            auto buf = std::make_unique<unsigned char[]>(len);
            writeString(buf.get(), source->read(buf.get(), len), to);
            to.flush();
        }

        else if (msg == STDERR_ERROR) {
            string error = readString(from);
            unsigned int status = readInt(from);
            return std::make_exception_ptr(Error(status, error));
        }

        else if (msg == STDERR_NEXT)
            printError(chomp(readString(from)));

        else if (msg == STDERR_START_ACTIVITY) {
            auto act = readNum<ActivityId>(from);
            auto lvl = (Verbosity) readInt(from);
            auto type = (ActivityType) readInt(from);
            auto s = readString(from);
            auto fields = readFields(from);
            auto parent = readNum<ActivityId>(from);
            logger->startActivity(act, lvl, type, s, fields, parent);
        }

        else if (msg == STDERR_STOP_ACTIVITY) {
            auto act = readNum<ActivityId>(from);
            logger->stopActivity(act);
        }

        else if (msg == STDERR_RESULT) {
            auto act = readNum<ActivityId>(from);
            auto type = (ResultType) readInt(from);
            auto fields = readFields(from);
            logger->result(act, type, fields);
        }

        else if (msg == STDERR_LAST)
            break;

        else
            throw Error("got unknown message type %x from Nix daemon", msg);
    }

    return nullptr;
}

void ConnectionHandle::withFramedSink(std::function<void(Sink &sink)> fun)
{
    (*this)->to.flush();

    std::exception_ptr ex;

    /* Handle log messages / exceptions from the remote on a
        separate thread. */
    std::thread stderrThread([&]()
    {
        try {
            processStderr(nullptr, nullptr, false);
        } catch (...) {
            ex = std::current_exception();
        }
    });

    Finally joinStderrThread([&]()
    {
        if (stderrThread.joinable()) {
            stderrThread.join();
            if (ex) {
                try {
                    std::rethrow_exception(ex);
                } catch (...) {
                    ignoreException();
                }
            }
        }
    });

    {
        FramedSink sink((*this)->to, ex);
        fun(sink);
        sink.flush();
    }

    stderrThread.join();
    if (ex)
        std::rethrow_exception(ex);

}

static RegisterStoreImplementation<UDSRemoteStore, UDSRemoteStoreConfig> regStore;

}
