#include "node.h"
#include "pnode.h"

namespace phxpaxos
{

int Node :: RunNode(const Options & oOptions, Node *& poNode)
{
    if (oOptions.bIsLargeValueMode)
    {
        InsideOptions::Instance()->SetAsLargeBufferMode();
    }
    
    InsideOptions::Instance()->SetGroupCount(oOptions.iGroupCount);
        
    poNode = nullptr;
    NetWork * poNetWork = nullptr;

    Breakpoint::m_poBreakpoint = nullptr;
    BP->SetInstance(oOptions.poBreakpoint);

    PNode * poRealNode = new PNode();
    int ret = poRealNode->Init(oOptions, poNetWork);
    if (ret != 0)
    {
        delete poRealNode;
        return ret;
    }

    //step1 set node to network
    //very important, let network on recieve callback can work.
    poNetWork->m_poNode = poRealNode;

    //step2 run network.
    //start recieve message from network, so all must init before this step.
    //must be the last step.
    poNetWork->RunNetWork();


    poNode = poRealNode;

    return 0;
}
    
}


