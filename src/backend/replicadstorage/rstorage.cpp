#include "rstorage.h"

#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "libpaxos/node.h"
#include "libpaxos/options.h"
#include "libpaxos/sm.h"

#include "rstorage/rsc.h"
#include "storage/threadpool_cpp.h"

static std::string ipPortList[] = {"10.77.110.144:60001", "10.77.110.144:60002", "10.77.110.144:60003"};
static std::string sIpPortList = ipPortList[0];

int parse_ipport(const char *pcStr, phxpaxos::NodeInfo &oNodeInfo) {
    char sIP[32] = {0};
    int iPort = -1;

    if (sscanf(pcStr, "%[^':']:%d", sIP, &iPort) != 2) {
        return -1;
    }

    oNodeInfo.SetIPPort(sIP, iPort);

    return 0;
}

int parse_ipport_list(const char *pcStr, phxpaxos::NodeInfoList &vecNodeInfoList) {
    std::string sTmpStr;
    int iStrLen = strlen(pcStr);

    for (int i = 0; i < iStrLen; i++) {
        if (pcStr[i] == ',' || i == iStrLen - 1) {
            if (i == iStrLen - 1 && pcStr[i] != ',') {
                sTmpStr += pcStr[i];
            }

            phxpaxos::NodeInfo oNodeInfo;
            if (parse_ipport(sTmpStr.data(), oNodeInfo) != 0) {
                return -1;
            }

            vecNodeInfoList.push_back(oNodeInfo);

            sTmpStr = "";
        } else {
            sTmpStr += pcStr[i];
        }
    }

    return 0;
}

class rstorageSMCtx {
public:
    rstorageSMCtx() = default;

    int pid;
};

int tempfunc() {
    return -1;
}

class rstorageSM : public phxpaxos::StateMachine {
public:
    rstorageSM() = default;
    ~rstorageSM() override = default;

    bool Execute(const int iGroupIdx, const uint64_t llInstanceID, const std::string &sPaxosValue, phxpaxos::SMCtx *poSMCtx) override {
        void *buffer;
        if (poSMCtx != nullptr && poSMCtx->m_pCtx != nullptr) {
            buffer = packjob(((rstorageSMCtx *) poSMCtx->m_pCtx)->pid, sPaxosValue.data(), sPaxosValue.size(), 1);
        } else {
            buffer = packjob(0, sPaxosValue.data(), sPaxosValue.size(), 0);
        }
        pool_add_worker(handle_process_kv_req, buffer);
        return true;
    }

    const int SMID() const override { return 1; }
};

static rstorageSM mysm;
static phxpaxos::Node *rnode;

void *echo(int pid, const char *message, int messageLen) {
    phxpaxos::SMCtx oCtx;
    rstorageSMCtx myCtx{pid};
    oCtx.m_iSMID = 1;
    oCtx.m_pCtx = (void *) &myCtx;
    uint64_t llInstanceID = 0;
    auto s = std::string(message, messageLen);
    rnode->Propose(0, s, llInstanceID, &oCtx);
    return nullptr;
}

int initReplicadStorage(int segidx) {
    for (int i = 1; i < 3; ++i) {
        sIpPortList += "," + ipPortList[i];
    }

    phxpaxos::NodeInfo oMyNode;
    if (parse_ipport(ipPortList[segidx].data(), oMyNode) != 0) {
        return -1;
    }

    phxpaxos::NodeInfoList vecNodeInfoList;
    if (parse_ipport_list(sIpPortList.data(), vecNodeInfoList) != 0) {
        return -1;
    }

    phxpaxos::Options options1;
    char sTmp[128] = {0};
    snprintf(sTmp, sizeof(sTmp), "./logpath_%s_%d", oMyNode.GetIP().c_str(), oMyNode.GetPort());
    options1.sLogStoragePath = std::string(sTmp);
    if (access(options1.sLogStoragePath.c_str(), F_OK) == -1) {
        if (mkdir(options1.sLogStoragePath.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) == -1) {
            printf("Create dir fail\n");
            return -1;
        }
    }
    options1.iGroupCount = 1;
    options1.oMyNode = oMyNode;
    options1.vecNodeInfoList = vecNodeInfoList;
    phxpaxos::GroupSMInfo smInfo;
    smInfo.iGroupIdx = 0;
    smInfo.bIsUseMaster = true;
    smInfo.vecSMList.push_back(&mysm);
    options1.vecGroupSMInfoList.push_back(smInfo);
    if (phxpaxos::Node::RunNode(options1, rnode)) {
        return -1;
    }
    return 0;
}
