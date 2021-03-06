#include "PacketParse.h"


CPacketParse::CPacketParse(void)
{
	//如果是包头模式，这里需要设置包头的长度
	m_u4PacketHead      = PACKET_HEAD;

	//这里修改属于你的包解析版本号
	sprintf_safe(m_szPacketVersion, MAX_BUFF_20, "0.90");

	//这里设置你的包模式
	m_u1PacketMode      = PACKET_WITHSTREAM;
}

CPacketParse::~CPacketParse(void)
{
}

void CPacketParse::Init()
{
	m_u4PacketHead      = PACKET_HEAD;
	m_u2PacketCommandID = 0;
	m_u4PacketData      = 0;
	m_u4HeadSrcSize     = 0;
	m_u4BodySrcSize     = 0;

	m_blIsHead          = false;

	m_pmbHead           = NULL;
	m_pmbBody           = NULL;
}

bool CPacketParse::SetPacketHead(uint32 u4ConnectID, ACE_Message_Block* pmbHead, IMessageBlockManager* pMessageBlockManager)
{
	//这里添加自己对包头的分析，主要分析出包长度。
	char* pData  = (char* )pmbHead->rd_ptr();
	uint32 u4Len = pmbHead->length();

	m_u4HeadSrcSize = u4Len;
	if(u4Len == sizeof(uint32))
	{
		memcpy_safe(pData, (uint32)sizeof(uint32), (char* )&m_u4PacketData, (uint32)sizeof(uint32));
		
		m_pmbHead = pmbHead;
		m_blIsHead = true;
		return true;
	}
	else
	{
		return false;
	}
}

bool CPacketParse::SetPacketBody(uint32 u4ConnectID, ACE_Message_Block* pmbBody, IMessageBlockManager* pMessageBlockManager)
{
	//这里分析出包体内的一些数据，如果包头包含了CommandID，那么包体就不必做解析。
	char* pData  = (char* )pmbBody->rd_ptr();
	uint32 u4Len = pmbBody->length();

	m_u4BodySrcSize = u4Len;
	if(u4Len >= sizeof(uint16))
	{
		memcpy_safe(pData, (uint32)sizeof(uint16), (char* )&m_u2PacketCommandID, (uint32)sizeof(uint16));
		m_blIsHead = false;
		m_pmbBody = pmbBody;
		return true;
	}
	else
	{
		m_blIsHead = false;
		return false;
	}
}



uint32 CPacketParse::MakePacketLength(uint32 u4ConnectID, uint32 u4DataLen, uint16 u2CommandID)
{
	if(u2CommandID == 0)
	{
		return 0;
	}
	else
	{
		return u4DataLen + sizeof(uint32);
	}
}

bool CPacketParse::MakePacket(uint32 u4ConnectID, const char* pData, uint32 u4Len, ACE_Message_Block* pMbData, uint16 u2CommandID)
{
	if(pMbData == NULL && u2CommandID == 0)
	{
		return false;
	}

	//拼装数据包
	memcpy_safe((char* )&u4Len, (uint32)sizeof(uint32), (char* )pMbData->wr_ptr(), (uint32)sizeof(uint32));
	pMbData->wr_ptr(sizeof(uint32));
	memcpy_safe((char* )pData, u4Len, pMbData->wr_ptr(), u4Len);
	pMbData->wr_ptr(u4Len);

	return true;
}



uint8 CPacketParse::GetPacketStream(uint32 u4ConnectID, ACE_Message_Block* pCurrMessage, IMessageBlockManager* pMessageBlockManager)
{
	//专门处理为数据流的数据包
	if(NULL == pCurrMessage || NULL == pMessageBlockManager)
	{
		return PACKET_GET_ERROR;
	}

	//首先判断是应该走握手包逻辑还是数据包逻辑
	_WebSocketInfo* pWebSocketInfo = App_WebSocketInfoManager::instance()->GetWebSocketInfo(u4ConnectID);
	if(NULL == pWebSocketInfo)
	{
		//没有找到连接相关信息，说明有错误，断开连接
		return PACKET_GET_ERROR;
	}

	if(pWebSocketInfo->m_emState == WEBSOCKET_STATE_HANDIN)
	{
		return WebSocketDisposeHandIn(pWebSocketInfo, pCurrMessage, pMessageBlockManager);
	}
	else
	{
		return WebSocketDisposeDataIn(pWebSocketInfo, pCurrMessage, pMessageBlockManager);
	}
}

bool CPacketParse::Connect(uint32 u4ConnectID, _ClientIPInfo objClientIPInfo, _ClientIPInfo objLocalIPInfo)
{
	//这里添加你对连接建立的逻辑处理，如果没有则不用在这里写任何代码
	//返回false，则连接会断开

	//查找当前连接记录里面有无当前记录
	return App_WebSocketInfoManager::instance()->Insert(u4ConnectID);
}

void CPacketParse::DisConnect(uint32 u4ConnectID)
{
	//这里添加你对连接断开的逻辑处理
	App_WebSocketInfoManager::instance()->Delete(u4ConnectID);
}

uint8 CPacketParse::WebSocketDisposeHandIn(_WebSocketInfo* pWebSocketInfo, ACE_Message_Block* pCurrMessage, IMessageBlockManager* pMessageBlockManager)
{
	//处理握手包
	char* pData   = pCurrMessage->rd_ptr();     //得到这个数据块的首字节
	uint32 u4Data = pCurrMessage->length();     //得到这个数据块的长度

	//将收到的数据粘入缓冲等待做切包处理
	if(pWebSocketInfo->m_u4DataLength + u4Data > MAX_DECRYPTLENGTH)
	{
		OUR_DEBUG((LM_ERROR, "[CPacketParse::WebSocketDisposeHandIn]pWebSocketInfo is full.\n"));
		return PACKET_GET_ERROR;
	}

	memcpy_safe(pData, u4Data, (char* )&pWebSocketInfo->m_szData[pWebSocketInfo->m_u4DataLength], u4Data);
	pWebSocketInfo->m_u4DataLength += u4Data;

	//判断是不是握手包的结束，找到末尾4个字符是不是\r\n\r\n
	if(pData[u4Data - 1] == '\n' && pData[u4Data - 2] == '\r'
		&& pData[u4Data - 3] == '\n' && pData[u4Data - 4] == '\r')
	{
		//接收到了完整的握手数据包，开始处理数据

		//申请一个包头，记录当前包的所有长度
		m_pmbHead = pMessageBlockManager->Create(sizeof(uint32));
		if(NULL == m_pmbHead)
		{
			OUR_DEBUG((LM_ERROR, "[CPacketParse::WebSocketDisposeHandIn]m_pmbHead is NULL.\n"));
			return PACKET_GET_ERROR;
		}

		uint32 u4NetPacketLen = pWebSocketInfo->m_u4DataLength;
		memcpy_safe((char*)&u4NetPacketLen, (uint32)sizeof(uint32), (char* )m_pmbHead->wr_ptr(), (uint32)sizeof(uint32));
		m_pmbHead->wr_ptr(sizeof(uint32));

		//获得包体
		m_pmbBody = pMessageBlockManager->Create(u4NetPacketLen);
		if(NULL == m_pmbBody)
		{
			OUR_DEBUG((LM_ERROR, "[CPacketParse::WebSocketDisposeHandIn]m_pmbBody is NULL.\n"));
			return PACKET_GET_ERROR;
		}

		memcpy_safe((char*)pWebSocketInfo->m_szData, pWebSocketInfo->m_u4DataLength, (char* )m_pmbBody->wr_ptr(), pWebSocketInfo->m_u4DataLength);
		m_pmbBody->wr_ptr(u4NetPacketLen);

		//设置命令字(0xe001指的是连接ID)
		m_u2PacketCommandID = 0xee01;

		//设置为数据包格式
		pWebSocketInfo->m_emState = WEBSOCKET_STATE_DATAIN;

		//处理完的数据从池中移除
		pCurrMessage->rd_ptr(u4Data);

		//设置缓冲处理完毕
		pWebSocketInfo->m_u4DataLength = 0;

		return (uint8)PACKET_GET_ENOUGTH;
	}
	else
	{
		//没有接收到完整的握手数据包，继续接收
		return (uint8)PACKET_GET_NO_ENOUGTH;
	}
}

uint8 CPacketParse::WebSocketDisposeDataIn(_WebSocketInfo* pWebSocketInfo, ACE_Message_Block* pCurrMessage, IMessageBlockManager* pMessageBlockManager)
{
	//处理数据包
	char* pData   = pCurrMessage->rd_ptr();     //得到这个数据块的首字节
	uint32 u4Data = pCurrMessage->length();     //得到这个数据块的长度
	uint32 u4PacketLen = 0;
	uint8 u1Ret        = (uint8)PACKET_GET_ERROR;  

	//将收到的数据粘入缓冲等待做切包处理
	if(pWebSocketInfo->m_u4DataLength + u4Data > MAX_ENCRYPTLENGTH)
	{
		OUR_DEBUG((LM_ERROR, "[CPacketParse::WebSocketDisposeHandIn]pWebSocketInfo is full.\n"));
		return (uint8)PACKET_GET_ERROR;
	}

	memcpy_safe(pData, u4Data, (char* )&pWebSocketInfo->m_szData[pWebSocketInfo->m_u4DataLength], u4Data);
	pWebSocketInfo->m_u4DataLength += u4Data;

	//解析规则约定为
	//<命令字>,<数据包长度><数据包体>

	//首先解密数据包,最大解密数据包是5K
	//如果再大，则扩展这个大小
	if(pWebSocketInfo->m_u4DecryptDataLen + pWebSocketInfo->m_u4DataLength > MAX_DECRYPTLENGTH)
	{
		OUR_DEBUG((LM_ERROR, "[CPacketParse::WebSocketDisposeHandIn]m_u4DecryptDataLen is full.\n"));
		return (uint8)PACKET_GET_ERROR;
	}

	char* pDecryptData = (char* )&pWebSocketInfo->m_szDecryptData[pWebSocketInfo->m_u4DecryptDataLen];
	uint32 u4DecryptLen = MAX_DECRYPTLENGTH;
	uint32 u4OriPacketLen = pWebSocketInfo->m_u4DataLength;
	u1Ret = Decrypt(pWebSocketInfo->m_szData, u4OriPacketLen, pDecryptData, u4DecryptLen);
	if(u1Ret != (uint8)PACKET_GET_ENOUGTH)
	{
		return u1Ret;
	}

	pWebSocketInfo->m_u4DecryptDataLen += u4DecryptLen;

	//如果接受字节少于9个，则认为包头不完整
	if(u4Data < 9)
	{
		return (uint8)PACKET_GET_NO_ENOUGTH;
	}

	//已经解析出完整数据包，判断是否有后续数据包的部分数据
	if(pWebSocketInfo->m_u4DataLength > u4OriPacketLen)
	{
		//有后续的数据包，在这里需要处理一下
		pWebSocketInfo->m_u4DataLength -= u4OriPacketLen;

		memcpy_safe((char* )&pWebSocketInfo->m_szData[u4OriPacketLen], pWebSocketInfo->m_u4DataLength, (char* )&pWebSocketInfo->m_szData, pWebSocketInfo->m_u4DataLength);
	}
	else
	{
		pWebSocketInfo->m_u4DataLength = 0;
	}
	
	//接收到了完整的数据包，开始处理数据
	//获得当前包ID和包长
	uint32 u4CurrDecryptDataLen = pWebSocketInfo->m_u4DecryptDataLen;
	u1Ret = ReadDataPacketInfo(pWebSocketInfo->m_szDecryptData, u4CurrDecryptDataLen, m_u2PacketCommandID, u4PacketLen);
	if(u1Ret == PACKET_GET_ENOUGTH)
	{
		//申请一个包头，记录当前包的所有长度
		m_pmbHead = pMessageBlockManager->Create(sizeof(uint32));
		if(NULL == m_pmbHead)
		{
			return (uint8)PACKET_GET_ERROR;
		}

		//去掉包头，只留数据体在包体内
		char* pInfo = (char* )ACE_OS::strstr(pWebSocketInfo->m_szDecryptData, "{");
		if(NULL == pInfo)
		{
			//如果找不到大括号，说明数据包异常，断开这个链接
			return (uint8)PACKET_GET_ERROR;
		}

		uint32 u4NetPacketLen = u4CurrDecryptDataLen - (uint32)(pInfo - pWebSocketInfo->m_szDecryptData);
		memcpy_safe((char*)&u4NetPacketLen, (uint32)sizeof(uint32), (char* )m_pmbHead->wr_ptr(), (uint32)sizeof(uint32));
		m_pmbHead->wr_ptr(sizeof(uint32));

		//获得包体
		m_pmbBody = pMessageBlockManager->Create(u4NetPacketLen);
		if(NULL == m_pmbBody)
		{
			return PACKET_GET_ERROR;
		}

		memcpy_safe((char*)pInfo, u4NetPacketLen, (char* )m_pmbBody->wr_ptr(), u4NetPacketLen);
		m_pmbBody->wr_ptr(u4NetPacketLen);

		//处理完的数据从块中移除
		pCurrMessage->reset();

		//如果有剩余数据，则再让系统判定一下
		if(pWebSocketInfo->m_u4DataLength > 0)
		{
			pCurrMessage->size((size_t)pWebSocketInfo->m_u4DataLength);
			memcpy_safe(pWebSocketInfo->m_szData, pWebSocketInfo->m_u4DataLength, (char* )pCurrMessage->wr_ptr(), pWebSocketInfo->m_u4DataLength);
			pCurrMessage->wr_ptr(pWebSocketInfo->m_u4DataLength);
			pWebSocketInfo->m_u4DataLength = 0;
		}

		//如果解析包还有内容，则再放回去
		pWebSocketInfo->m_u4DecryptDataLen -= u4CurrDecryptDataLen;
		if(pWebSocketInfo->m_u4DecryptDataLen > 0)
		{
			memcpy_safe((char* )&pWebSocketInfo->m_szDecryptData[u4CurrDecryptDataLen],
				pWebSocketInfo->m_u4DecryptDataLen,
				pWebSocketInfo->m_szDecryptData,
				pWebSocketInfo->m_u4DecryptDataLen);
		}

		return (uint8)PACKET_GET_ENOUGTH;
	}
	else
	{
		return u1Ret;
	}

}

uint8 CPacketParse::Decrypt(char* pOriData, uint32& u4Len, char* pEncryData, uint32& u4EncryLen)
{
	//帧头长度，默认是6,扩展后是8
	int nFrameLen = 6;

	if(u4Len < 6)
	{
		m_blIsHead = true;

		return PACKET_GET_NO_ENOUGTH;
	}

	unsigned char* mp_mask_byte = NULL;	
	int nMinExpectedSize = 0;


	unsigned char payloadFlags = pOriData[0];
	if (payloadFlags != 129)
		return PACKET_GET_ERROR;

	unsigned char basicSize = pOriData[1] & 0x7F;
	unsigned int payloadSize;
	int masksOffset;

	if (basicSize <= 125)
	{
		payloadSize = basicSize;
		masksOffset = 2;
	}
	else if (basicSize == 126)
	{
		nMinExpectedSize += 2;
		if (u4Len < (uint32)nMinExpectedSize)
		{
			m_blIsHead = true;
			return PACKET_GET_NO_ENOUGTH;
		}
		payloadSize = ntohs( *(u_short*) (pOriData + 2) );
		masksOffset = 4;
		nFrameLen   = 8;
	}
	else if (basicSize == 127)
	{
		nMinExpectedSize += 8;
		if (u4Len < (uint32)nMinExpectedSize)
		{
			m_blIsHead = true;

			return PACKET_GET_NO_ENOUGTH;
		}
		payloadSize = ntohl( *(u_long*) (pOriData + 2) );
		masksOffset = 6;
		nFrameLen   = 10;
	}
	else
	{
		return PACKET_GET_ERROR;
	}

	nMinExpectedSize += payloadSize;
	if (u4Len < (uint32)nMinExpectedSize)
	{
		m_blIsHead = true;

		return PACKET_GET_NO_ENOUGTH;
	}

	char masks[4];
	memcpy_safe(pOriData + masksOffset, 4, (char* )masks, 4);

	memcpy_safe((char* )(pOriData + masksOffset + 4), payloadSize, pEncryData, payloadSize);
	for (unsigned int i = 0; i < payloadSize; i++) {
		pEncryData[i] = (pEncryData[i] ^ masks[i%4]);
	}
	pEncryData[payloadSize] = '\0';

	if(u4EncryLen < payloadSize)
	{
		//如果字符串大于最大包体，则返回包错误。
		return PACKET_GET_ERROR;
	}

	u4EncryLen = payloadSize;
	//这里6个字节头是固定的，第一个字节固定的-127,后面根据数据的长度或者4或者6字节
	if(payloadSize + nFrameLen > u4Len)
	{
		//说明解析不正常，解析后的数据长度和当前数据长度不相符
		return PACKET_GET_ERROR;
	}

	u4Len      = payloadSize + nFrameLen;

	return PACKET_GET_ENOUGTH;
}

uint8 CPacketParse::ReadDataPacketInfo(const char* pData, uint32& u4DataLen, uint16& u2CommandID, uint32& u4PacketLen)
{
	uint32 u4AllDataCount = u4DataLen;

	char* pInfo = NULL;
	char szTemp[MAX_BUFF_100] = {'\0'};

	//寻找第一个{，用于界定数据包信息的长度
	pInfo = (char* )ACE_OS::strstr(pData, "{");
	if(NULL == pInfo)
	{
		//如果找不到大括号，说明数据包异常，断开这个链接
		return (uint8)PACKET_GET_ERROR;
	}

	if((int)(pInfo - pData) == 0)
	{
		//找不到前面的数据头，返回失败
		return (uint8)PACKET_GET_ERROR;
	}

	//获得命令字符串
	memcpy_safe((char* )pData, (uint32)(pInfo - pData), szTemp, (uint32)MAX_BUFF_100);

	//如果找到了，开始切分数据，获得数据长度和命令ID
	char* pCommand = NULL;
	pCommand = (char* )ACE_OS::strstr(szTemp, ",");
	if(NULL == pInfo)
	{
		//如果找不到逗号，说明这个命令包是错误的，断开连接
		return (uint8)PACKET_GET_ERROR;
	}
	else
	{
		char szTemp2[MAX_BUFF_50] = {'\0'}; 

		if(pCommand - szTemp == 0)
		{
			//说明没有找到数据头，返回失败
			return (uint8)PACKET_GET_ERROR;
		}

		memcpy_safe((char* )pData, (uint32)(pCommand - szTemp), szTemp2, (uint32)MAX_BUFF_50);
		//得到命令字和数据包长度
		//u2CommandID = (uint16)ACE_OS::atoi(szTemp2);
		//十六进制字符串转成整形
		int nCommandID = 0;
		sscanf(szTemp2, "%x", &nCommandID);
		m_u2PacketCommandID = (uint16)nCommandID;
		memcpy_safe((char* )(pCommand + 1), (uint32)(ACE_OS::strlen(szTemp) - (int)(pCommand - szTemp) - 1),
			szTemp2, 
			(uint32)MAX_BUFF_50);
		szTemp2[ACE_OS::strlen(szTemp) - (int)(pCommand - szTemp) - 1] = '\0';
		u4PacketLen = (uint32)ACE_OS::atoi(szTemp2);
		
		if(u4PacketLen == 0 || u2CommandID == 0)
		{
			//解析出的命令字和长度有一个不对就断开连接
			return (uint8)PACKET_GET_ERROR;
		}
		else
		{
			//判断包是否接收完整了
			if(u4PacketLen > u4AllDataCount)
			{
				return (uint8)PACKET_GET_NO_ENOUGTH;
			}
			else
			{
				u4DataLen = u4PacketLen;
				return (uint8)PACKET_GET_ENOUGTH;
			}
		}
	}
}

void CPacketParse::GetPacketHeadInfo( _PacketHeadInfo& objPacketHeadInfo )
{
}