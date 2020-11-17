#include "network.h"
#include "node.h"
#include "commdef.h"

namespace phxpaxos
{

NetWork :: NetWork() : m_poNode(nullptr)
{
}
    
int NetWork :: OnReceiveMessage(const char * pcMessage, const int iMessageLen)
{
    if (m_poNode != nullptr)
    {
        m_poNode->OnReceiveMessage(pcMessage, iMessageLen);
    }
    else
    {
        PLHead("receive msglen %d", iMessageLen);
    }

    return 0;
}

}


