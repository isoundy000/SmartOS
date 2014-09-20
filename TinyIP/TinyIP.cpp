﻿#include "TinyIP.h"

#define NET_DEBUG DEBUG

const byte g_FullMac[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }; // FF-FF-FF-FF-FF-FF
const byte g_ZeroMac[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }; // 00-00-00-00-00-00

TinyIP::TinyIP(ITransport* port, IPAddress ip, byte mac[6])
{
	_port = port;
	_StartTime = Time.Current();

	/*MacAddress addr1 = *(ulong*)g_FullMac;
	MacAddress addr2 = addr1;
	ulong vv = addr2;*/
	
	//const byte defip[] = {192, 168, 0, 1};
	const IPAddress defip = 0x0100A8C0;
	if(ip)
		//memcpy(IP, ip, 4);
		IP = ip;
	else
	{
		// 随机IP，取ID最后一个字节
		//memcpy(IP, defip, 3);
		//IP[3] = Sys.ID[0];
		IP = defip & 0x00FFFFFF;
		byte first = Sys.ID[0];
		if(first <= 1 || first >= 254) first = 2;
		IP |= first << 24;
	}

	if(mac)
		memcpy(Mac, mac, 6);
	else
	{
		// 随机Mac，前三个字节取自YWS的ASCII，最后3个字节取自后三个ID
		Mac[0] = 'Y'; Mac[1] = 'W'; Mac[2] = 'S';
		//memcpy(Mac, "YWS", 3);
		memcpy(&Mac[3], (byte*)Sys.ID, 6 - 3);
		// MAC地址首字节奇数表示组地址，这里用偶数
		//Mac[0] = 'N'; Mac[1] = 'X';
		//memcpy(&Mac[2], (byte*)Sys.ID, 6 - 2);
	}

	/*const byte mask[] = {0xFF, 0xFF, 0xFF, 0};
	memcpy(Mask, mask, 4);
	memcpy(DHCPServer, defip, 4);
	memcpy(Gateway, defip, 4);
	memcpy(DNSServer, defip, 4);*/
	Mask = 0x00FFFFFF;
	DHCPServer = Gateway = DNSServer = defip;

	Buffer = NULL;
	BufferSize = 1500;
	EnableBroadcast = true;

	Sockets.SetCapacity(0x10);
	//Arp = NULL;
	// 必须有Arp，否则无法响应别人的IP询问
	Arp = new ArpSocket(this);
}

TinyIP::~TinyIP()
{
	if(_port) delete _port;
    _port = NULL;

	if(Buffer) delete Buffer;
	Buffer = NULL;

	/*if(_net) delete _net;
	_net = NULL;*/
	
	if(Arp) delete Arp;
	Arp = NULL;
}

// 循环调度的任务
uint TinyIP::Fetch(byte* buf, uint len)
{
	if(!buf) buf = Buffer;
	if(!len) len = BufferSize;

	// 获取缓冲区的包
	len = _port->Read(buf, len);
	// 如果缓冲器里面没有数据则转入下一次循环
	if(len < sizeof(ETH_HEADER)/* || !_net->Unpack(len)*/) return 0;

	ETH_HEADER* eth = (ETH_HEADER*)buf;
	// 只处理发给本机MAC的数据包。此时进行目标Mac地址过滤，可能是广播包
	if(memcmp(eth->DestMac, Mac, 6) != 0
	&& memcmp(eth->DestMac, g_FullMac, 6) != 0
	&& memcmp(eth->DestMac, g_ZeroMac, 6) != 0
	) return 0;

	return len;
}

void TinyIP::Process(MemoryStream* ms)
{
	if(!ms) return;

	//ETH_HEADER* eth = (ETH_HEADER*)ms->Current();
	ETH_HEADER* eth = ms->Retrieve<ETH_HEADER>();
	if(!eth) return;
#if NET_DEBUG
	/*debug_printf("Ethernet 0x%04X ", eth->Type);
	ShowMac(eth->SrcMac);
	debug_printf(" => ");
	ShowMac(eth->DestMac);
	debug_printf("\r\n");*/
#endif

	// 只处理发给本机MAC的数据包。此时不能进行目标Mac地址过滤，因为可能是广播包
	//if(memcmp(eth->DestMac, Mac, 6) != 0) return;
	// 这里复制Mac地址
	memcpy(LocalMac, eth->DestMac, 6);
	memcpy(RemoteMac, eth->SrcMac, 6);

	// 处理ARP
	if(eth->Type == ETH_ARP)
	{
		if(Arp) Arp->Process(ms);

		return;
	}

	IP_HEADER* ip = ms->Retrieve<IP_HEADER>();
	// 是否发给本机。注意memcmp相等返回0
	if(!ip || !IsMyIP(ip->DestIP)) return;

#if NET_DEBUG
	if(eth->Type != ETH_IP)
	{
		debug_printf("Unkown EthernetType 0x%02X From", eth->Type);
		ShowIP(ip->SrcIP);
		debug_printf("\r\n");
	}
#endif

	// 记录远程信息
	//memcpy(LocalIP, ip->DestIP, 4);
	//memcpy(RemoteIP, ip->SrcIP, 4);
	LocalIP = ip->DestIP;
	RemoteIP = ip->SrcIP;

	//!!! 太杯具了，收到的数据包可能有多余数据，这两个长度可能不等
	//assert_param(__REV16(ip->TotalLength) == len);
	// 数据包是否完整
	//if(ms->Remain() < __REV16(ip->TotalLength)) return;
	// 计算负载数据的长度，注意ip可能变长，长度Length的单位是4字节
	//len -= sizeof(IP_HEADER);

	// 前面的len不准确，必须以这个为准
	uint size = __REV16(ip->TotalLength) - (ip->Length << 2);
	ms->Length = ms->Position + size;
	//len = size;
	//buf += (ip->Length << 2);
	ms->Position += (ip->Length << 2) - sizeof(IP_HEADER);

	// 各处理器有可能改变数据流游标，这里备份一下
	uint p = ms->Position;
	//for(int i=0; i < Sockets.Count(); i++)
	// 考虑到可能有通用端口处理器，也可能有专用端口处理器（一般在后面），这里偷懒使用倒序处理
	uint count = Sockets.Count();
	for(int i=count-1; i>=0; i--)
	{
		Socket* socket = Sockets[i];
		if(socket)
		{
			// 必须类型匹配
			if(socket->Type == ip->Protocol)
			{
				// 如果处理成功，则中断遍历
				if(socket->Process(ms)) return;
				ms->Position = p;
			}
		}
	}

#if NET_DEBUG
	debug_printf("IP Unkown Protocol=%d ", ip->Protocol);
	ShowIP(ip->SrcIP);
	debug_printf(" => ");
	ShowIP(ip->DestIP);
	debug_printf("\r\n");
#endif
}

// 任务函数
void TinyIP::Work(void* param)
{
	TinyIP* tip = (TinyIP*)param;
	if(tip)
	{
		uint len = tip->Fetch();
		if(len)
		{
#if NET_DEBUG
			//ulong start = Time.Current();
#endif

			//tip->Process(tip->Buffer, tip->BufferSize, 0, len);
			MemoryStream ms(tip->Buffer, tip->BufferSize);
			ms.Length = len;
			tip->Process(&ms);
#if NET_DEBUG
			//uint cost = Time.Current() - start;
			//debug_printf("TinyIP::Process cost %d us\r\n", cost);
#endif
		}
	}
}

bool TinyIP::Open()
{
	if(_port->Open()) return true;

	debug_printf("TinyIP Init Failed!\r\n");
	return false;
}

bool TinyIP::Init()
{
#if NET_DEBUG
	debug_printf("\r\nTinyIP Init...\r\n");
	//uint us = Time.Current();
#endif

	// 分配缓冲区。比较大，小心堆空间不够
	if(!Buffer)
	{
		Buffer = new byte[BufferSize];
		assert_param(Buffer);
		assert_param(Sys.CheckMemory());

		// 首次使用时初始化缓冲区
		//if(!_net) _net = new NetPacker(Buffer);
	}

	if(!Open()) return false;

#if NET_DEBUG
	debug_printf("\tIP:\t");
	ShowIP(IP);
	debug_printf("\r\n\tMask:\t");
	ShowIP(Mask);
	debug_printf("\r\n\tGate:\t");
	ShowIP(Gateway);
	debug_printf("\r\n\tDHCP:\t");
	ShowIP(DHCPServer);
	debug_printf("\r\n\tDNS:\t");
	ShowIP(DNSServer);
	debug_printf("\r\n");
#endif

	// 添加到系统任务，马上开始，尽可能多被调度
    Sys.AddTask(Work, this);

#if NET_DEBUG
	uint us = Time.Current() - _StartTime;
	debug_printf("TinyIP Ready! Cost:%dms\r\n\r\n", us / 1000);
#endif

	return true;
}

void TinyIP::SendEthernet(ETH_TYPE type, byte* buf, uint len)
{
	ETH_HEADER* eth = (ETH_HEADER*)(buf - sizeof(ETH_HEADER));
	assert_param(IS_ETH_TYPE(type));

	eth->Type = type;
	memcpy(eth->DestMac, RemoteMac, 6);
	memcpy(eth->SrcMac, Mac, 6);

	len += sizeof(ETH_HEADER);
	//if(len < 60) len = 60;	// 以太网最小包60字节

	/*debug_printf("SendEthernet: type=0x%04x, len=%d\r\n", type, len);
	Sys.ShowHex((byte*)eth, len, '-');
	debug_printf("\r\n");*/

	//assert_param((byte*)eth == Buffer);
	_port->Write((byte*)eth, len);
}

void TinyIP::SendIP(IP_TYPE type, byte* buf, uint len)
{
	IP_HEADER* ip = (IP_HEADER*)(buf - sizeof(IP_HEADER));
	assert_param(ip);
	assert_param(IS_IP_TYPE(type));

	//memcpy(ip->DestIP, RemoteIP, 4);
	//memcpy(ip->SrcIP, IP, 4);
	ip->DestIP = RemoteIP;
	ip->SrcIP = IP;

	ip->Version = 4;
	//ip->TypeOfService = 0;
	ip->Length = sizeof(IP_HEADER) / 4;	// 暂时不考虑附加数据
	ip->TotalLength = __REV16(sizeof(IP_HEADER) + len);
	ip->Flags = 0x40;
	ip->FragmentOffset = 0;
	ip->TTL = 64;
	ip->Protocol = type;

	// 网络序是大端
	ip->Checksum = 0;
	ip->Checksum = __REV16((ushort)TinyIP::CheckSum((byte*)ip, sizeof(IP_HEADER), 0));

	SendEthernet(ETH_IP, (byte*)ip, sizeof(IP_HEADER) + len);
}

bool IcmpSocket::Process(MemoryStream* ms)
{
	ICMP_HEADER* icmp = ms->Retrieve<ICMP_HEADER>();
	if(!icmp) return false;

	uint len = ms->Remain();
	if(OnPing)
	{
		// 返回值指示是否向对方发送数据包
		bool rs = OnPing(this, icmp, icmp->Next(), len);
		if(!rs) return true;
	}
	else
	{
#if NET_DEBUG
		if(icmp->Type != 0)
			debug_printf("Ping From "); // 打印发方的ip
		else
			debug_printf("Ping Reply "); // 打印发方的ip
		Tip->ShowIP(Tip->RemoteIP);
		debug_printf(" Payload=%d ", len);
		// 越过2个字节标识和2字节序列号
		debug_printf("ID=0x%04X Seq=0x%04X ", __REV16(icmp->Identifier), __REV16(icmp->Sequence));
		Sys.ShowString(icmp->Next(), len);
		debug_printf(" \r\n");
#endif
	}

	// 只处理ECHO请求
	if(icmp->Type != 8) return true;

	icmp->Type = 0; // 响应
	// 因为仅仅改变类型，因此我们能够提前修正校验码
	icmp->Checksum += 0x08;

	// 这里不能直接用sizeof(ICMP_HEADER)，而必须用len，因为ICMP包后面一般有附加数据
    Tip->SendIP(IP_ICMP, (byte*)icmp, icmp->Size() + len);

	return true;
}

// Ping目的地址，附带a~z重复的负载数据
bool IcmpSocket::Ping(IPAddress ip, uint payloadLength)
{
	assert_param(Tip->Arp);
	const byte* mac = Tip->Arp->Resolve(ip);
	if(!mac)
	{
#if NET_DEBUG
		debug_printf("No Mac For ");
		Tip->ShowIP(ip);
		debug_printf("\r\n");
#endif
		return false;
	}

	memcpy(Tip->RemoteMac, mac, 6);
	//memcpy(Tip->RemoteIP, ip, 4);
	Tip->RemoteIP = ip;

	byte buf[sizeof(ETH_HEADER) + sizeof(IP_HEADER) + sizeof(ICMP_HEADER) + 64];
	uint bufSize = ArrayLength(buf);
	MemoryStream ms(buf, bufSize);

	ETH_HEADER* eth = (ETH_HEADER*)buf;
	IP_HEADER* _ip = (IP_HEADER*)eth->Next();
	ICMP_HEADER* icmp = (ICMP_HEADER*)_ip->Next();
	icmp->Init(true);

	uint count = 0;

	icmp->Type = 8;
	icmp->Code = 0;

	byte* data = icmp->Next();
	for(int i=0, k=0; i<payloadLength; i++, k++)
	{
		if(k >= 23) k-=23;
		*data++ = ('a' + k);
	}

	//ushort now = Time.Current() / 1000;
	ushort now = Time.Current() >> 10;
	ushort id = __REV16(Sys.ID[0]);
	ushort seq = __REV16(now);
	icmp->Identifier = id;
	icmp->Sequence = seq;

	icmp->Checksum = 0;
	icmp->Checksum = __REV16((ushort)TinyIP::CheckSum((byte*)icmp, sizeof(ICMP_HEADER) + payloadLength, 0));

#if NET_DEBUG
	debug_printf("Ping ");
	Tip->ShowIP(ip);
	debug_printf(" with Identifier=0x%04x Sequence=0x%04x\r\n", id, seq);
#endif
	Tip->SendIP(IP_ICMP, buf, sizeof(ICMP_HEADER) + payloadLength);

	// 总等待时间
	//ulong end = Time.Current() + 1 * 1000000;
	//while(end > Time.Current())
	TimeWheel tw(1, 0, 0);
	while(!tw.Expired())
	{
		// 阻塞其它任务，频繁调度OnWork，等待目标数据
		uint len = Tip->Fetch(buf, bufSize);
		if(!len)
		{
			Sys.Sleep(2);	// 等待一段时间，释放CPU

			continue;
		}

		if(eth->Type == ETH_IP && _ip->Protocol == IP_ICMP)
		{
			if(icmp->Identifier == id && icmp->Sequence == seq
			//&& memcmp(_ip->DestIP, Tip->IP, 4) == 0
			//&& memcmp(_ip->SrcIP, ip, 4) == 0)
			&& _ip->DestIP == Tip->IP
			&& _ip->SrcIP == ip)
			{
				count++;
				break;
			}
		}

		// 用不到数据包交由系统处理
		//Tip->Process(buf, bufSize, 0, len);
		ms.Position = 0;
		ms.Length = len;
		Tip->Process(&ms);
	}

	return count;
}

TcpSocket::TcpSocket(TinyIP* tip) : Socket(tip)
{
	Type = IP_TCP;

	Port = 0;
	//*(uint*)RemoteIP = 0;
	RemoteIP = 0;
	RemotePort = 0;
	seqnum = 0xa;
}

bool TcpSocket::Process(MemoryStream* ms)
{
	TCP_HEADER* tcp = (TCP_HEADER*)ms->Current();
	if(!ms->TrySeek(tcp->Length << 2)) return false;
	
	Header = tcp;
	uint len = ms->Remain();

	ushort port = __REV16(tcp->DestPort);
	ushort remotePort = __REV16(tcp->SrcPort);

	// 仅处理本连接的IP和端口
	if(Port != 0 && port != Port) return false;
	if(RemotePort != 0 && remotePort != RemotePort) return false;
	//if(memcmp(Tip->RemoteIP, RemoteIP, 4) != 0) return false;
	/*uint rip = *(uint*)RemoteIP;
	if(rip != 0 && *(uint*)Tip->RemoteIP != rip) return false;*/
	if(RemoteIP != 0 && Tip->RemoteIP != RemoteIP) return false;

	// 不能修改主监听Socket的端口，否则可能导致收不到后续连接数据
	//Port = port;
	//RemotePort = remotePort;
	Tip->Port = port;
	Tip->RemotePort = remotePort;

	// 第一次同步应答
	if (tcp->Flags & TCP_FLAGS_SYN && !(tcp->Flags & TCP_FLAGS_ACK)) // SYN连接请求标志位，为1表示发起连接的请求数据包
	{
		if(OnAccepted)
			OnAccepted(this, tcp, tcp->Next(), len);
		else
		{
#if NET_DEBUG
			debug_printf("Tcp Accept "); // 打印发送方的ip
			TinyIP::ShowIP(Tip->RemoteIP);
			debug_printf("\r\n");
#endif
		}

		//第二次同步应答
		Head(1, true, false);

		// 需要用到MSS，所以采用4个字节的可选段
		//Send(tcp, 4, TCP_FLAGS_SYN | TCP_FLAGS_ACK);
		// 注意tcp->Size()包括头部的扩展数据，这里不用单独填4
		Send(tcp, 0, TCP_FLAGS_SYN | TCP_FLAGS_ACK);

		return true;
	}
	// 第三次同步应答,三次应答后方可传输数据
	if (tcp->Flags & TCP_FLAGS_ACK) // ACK确认标志位，为1表示此数据包为应答数据包
	{
		// 无数据返回ACK
		if (len == 0)
		{
			if (tcp->Flags & (TCP_FLAGS_FIN | TCP_FLAGS_RST))      //FIN结束连接请求标志位。为1表示是结束连接的请求数据包
			{
				Head(1, false, true);
				Send(tcp, 0, TCP_FLAGS_ACK);
			}
			return true;
		}

		if(OnReceived)
		{
			// 返回值指示是否向对方发送数据包
			bool rs = OnReceived(this, tcp, tcp->Next(), len);
			if(!rs)
			{
				// 发送ACK，通知已收到
				Head(1, false, true);
				Send(tcp, 0, TCP_FLAGS_ACK);
				return true;
			}
		}
		else
		{
#if NET_DEBUG
			debug_printf("Tcp Data(%d) From ", len);
			TinyIP::ShowIP(RemoteIP);
			debug_printf(" : ");
			//Sys.ShowString(_net->Payload, len);
			Sys.ShowString(tcp->Next(), len);
			debug_printf("\r\n");
#endif
		}
		// 发送ACK，通知已收到
		Head(len, false, true);
		//Send(buf, 0, TCP_FLAGS_ACK);

		//TcpSend(buf, len);

		// 响应Ack和发送数据一步到位
		Send(tcp, len, TCP_FLAGS_ACK | TCP_FLAGS_PUSH);
	}
	else if(tcp->Flags & (TCP_FLAGS_FIN | TCP_FLAGS_RST))
	{
		if(OnDisconnected) OnDisconnected(this, tcp, tcp->Next(), len);

		// RST是对方紧急关闭，这里啥都不干
		if(tcp->Flags & TCP_FLAGS_FIN)
		{
			Head(1, false, true);
			//Close(tcp, 0);
			Send(tcp, 0, TCP_FLAGS_ACK | TCP_FLAGS_PUSH | TCP_FLAGS_FIN);
		}
	}

	return true;
}

void TcpSocket::Send(TCP_HEADER* tcp, uint len, byte flags)
{
	tcp->SrcPort = __REV16(Port > 0 ? Port : Tip->Port);
	tcp->DestPort = __REV16(RemotePort > 0 ? RemotePort : Tip->RemotePort);
    tcp->Flags = flags;
	if(tcp->Length < sizeof(TCP_HEADER) / 4) tcp->Length = sizeof(TCP_HEADER) / 4;

	// 网络序是大端
	tcp->Checksum = 0;
	tcp->Checksum = __REV16((ushort)TinyIP::CheckSum((byte*)tcp - 8, 8 + sizeof(TCP_HEADER) + len, 2));

	// 注意tcp->Size()包括头部的扩展数据
	Tip->SendIP(IP_TCP, (byte*)tcp, tcp->Size() + len);
}

void TcpSocket::Head(uint ackNum, bool mss, bool opSeq)
{
    /*
	第一次握手：主机A发送位码为SYN＝1，随机产生Seq=1234567的数据包到服务器，主机B由SYN=1知道，A要求建立联机
	第二次握手：主机B收到请求后要确认联机信息，向A发送Ack=(主机A的Seq+1)，SYN=1，Ack=1，随机产生Seq=7654321的包
	第三次握手：主机A收到后检查Ack是否正确，即第一次发送的Seq+1，以及位码Ack是否为1，
	若正确，主机A会再发送Ack=(主机B的Seq+1)，Ack=1，主机B收到后确认Seq值与Ack=1则连接建立成功。
	完成三次握手，主机A与主机B开始传送数据。
	*/
	TCP_HEADER* tcp = Header;
	int ack = tcp->Ack;
	tcp->Ack = __REV(__REV(tcp->Seq) + ackNum);
    if (!opSeq)
    {
		// 我们仅仅递增第二个字节，这将允许我们以256或者512字节来发包
		tcp->Seq = __REV(seqnum << 8);
        // step the inititial seq num by something we will not use
        // during this tcp session:
        seqnum += 2;
		/*tcp->Seq = __REV(seqnum);
		seqnum++;*/
		//tcp->Seq = 0;
    }else
	{
		tcp->Seq = ack;
	}

	tcp->Length = sizeof(TCP_HEADER) / 4;
    // 头部后面可能有可选数据，Length决定头部总长度（4的倍数）
    if (mss)
    {
		uint* p = (uint*)tcp->Next();
        // 使用可选域设置 MSS 到 1460:0x5b4
		*p++ = __REV(0x020405b4);
		//*p++ = __REV(0x01030302);
		//*p++ = __REV(0x01010402);

		tcp->Length += 1;
    }
}

void TcpSocket::Ack(uint len)
{
	TCP_HEADER* tcp = (TCP_HEADER*)(Tip->Buffer + sizeof(ETH_HEADER) + sizeof(IP_HEADER));
	tcp->Init(true);
	Send(tcp, len, TCP_FLAGS_ACK | TCP_FLAGS_PUSH);
}

void TcpSocket::Close()
{
	TCP_HEADER* tcp = (TCP_HEADER*)(Tip->Buffer + sizeof(ETH_HEADER) + sizeof(IP_HEADER));
	tcp->Init(true);
	Send(tcp, 0, TCP_FLAGS_ACK | TCP_FLAGS_PUSH | TCP_FLAGS_FIN);
}

void TcpSocket::Send(byte* buf, uint len)
{
	TCP_HEADER* tcp = (TCP_HEADER*)(Tip->Buffer + sizeof(ETH_HEADER) + sizeof(IP_HEADER));
	tcp->Init(true);
	byte* end = Tip->Buffer + Tip->BufferSize;
	if(buf < tcp->Next() || buf >= end)
	{
		// 复制数据，确保数据不会溢出
		uint len2 = Tip->BufferSize - tcp->Offset() - tcp->Size();
		assert_param(len <= len2);

		memcpy(tcp->Next(), buf, len);
	}

	Send(tcp, len, TCP_FLAGS_PUSH);
}

bool UdpSocket::Process(MemoryStream* ms)
{
	UDP_HEADER* udp = ms->Retrieve<UDP_HEADER>();
	if(!udp) return false;

	ushort port = __REV16(udp->DestPort);
	ushort remotePort = __REV16(udp->SrcPort);

	// 仅处理本连接的IP和端口
	if(Port != 0 && port != Port) return false;

	Tip->Port = port;
	Tip->RemotePort = remotePort;

	byte* data = udp->Next();
	uint len = ms->Remain();
	uint plen = __REV16(udp->Length);
	assert_param(len + sizeof(UDP_HEADER) == plen);

	if(OnReceived)
	{
		// 返回值指示是否向对方发送数据包
		bool rs = OnReceived(this, udp, data, len);
		if(!rs) return true;
	}
	else
	{
#if NET_DEBUG
		debug_printf("UDP ");
		Tip->ShowIP(Tip->RemoteIP);
		debug_printf(":%d => ", Tip->RemotePort);
		Tip->ShowIP(Tip->LocalIP);
		debug_printf(":%d Payload=%d udp_len=%d \r\n", port, len, plen);

		Sys.ShowString(data, len);
		debug_printf(" \r\n");
#endif
	}

	Send(data, len, false);

	return true;
}

void UdpSocket::Send(byte* buf, uint len, bool checksum)
{
	UDP_HEADER* udp = (UDP_HEADER*)(buf - sizeof(UDP_HEADER));
	assert_param(udp);

	udp->SrcPort = __REV16(Tip->Port);
	udp->DestPort = __REV16(Tip->RemotePort);
	udp->Length = __REV16(sizeof(UDP_HEADER) + len);

	// 网络序是大端
	udp->Checksum = 0;
	if(checksum) udp->Checksum = __REV16((ushort)TinyIP::CheckSum((byte*)udp, sizeof(UDP_HEADER) + len, 1));

	//assert_param(_net->IP);
	Tip->SendIP(IP_UDP, (byte*)udp, sizeof(UDP_HEADER) + len);
}

#define TinyIP_HELP
#ifdef TinyIP_HELP
void TinyIP::ShowIP(IPAddress ip)
{
	byte* ips = (byte*)&ip;
	debug_printf("%d", *ips++);
	for(int i=1; i<4; i++)
		debug_printf(".%d", *ips++);
}

void TinyIP::ShowMac(const byte mac[6])
{
	debug_printf("%02X", *mac++);
	for(int i=1; i<6; i++)
		debug_printf("-%02X", *mac++);
}

uint TinyIP::CheckSum(byte* buf, uint len, byte type)
{
    // type 0=ip
    //      1=udp
    //      2=tcp
    unsigned long sum = 0;

    if(type == 1)
    {
        sum += IP_UDP; // protocol udp
        // the length here is the length of udp (data+header len)
        // =length given to this function - (IP.scr+IP.dst length)
        sum += len - 8; // = real tcp len
    }
    if(type == 2)
    {
        sum += IP_TCP;
        // the length here is the length of tcp (data+header len)
        // =length given to this function - (IP.scr+IP.dst length)
        sum += len - 8; // = real tcp len
    }
    // build the sum of 16bit words
    while(len > 1)
    {
        sum += 0xFFFF & (*buf << 8 | *(buf + 1));
        buf += 2;
        len -= 2;
    }
    // if there is a byte left then add it (padded with zero)
    if (len)
    {
        sum += (0xFF & *buf) << 8;
    }
    // now calculate the sum over the bytes in the sum
    // until the result is only 16bit long
    while (sum>>16)
    {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    // build 1's complement:
    return( (uint) sum ^ 0xFFFF);
}

bool TinyIP::IsMyIP(IPAddress ip)
{
	//int i = 0;
	//for(i = 0; i < 4 && ip[i] == IP[i]; i++);
	//if(i == 4) return true;
	//if(*(uint*)ip == *(uint*)IP) return true;
	if(ip == IP) return true;

	if(EnableBroadcast && IsBroadcast(ip)) return true;

	return false;
}

bool TinyIP::IsBroadcast(IPAddress ip)
{
	//int i = 0;
	// 全网广播
	//for(i = 0; i < 4 && ip[i] == 0xFF; i++);
	//if(i == 4) return true;
	//if(*(uint*)ip == 0xFFFFFFFF) return true;
	if(ip == 0xFFFFFFFF) return true;

	// 子网广播。网络位不变，主机位全1
	//for(i = 0; i < 4 && ip[i] == (IP[i] | ~Mask[i]); i++);
	//if(i == 4) return true;
	//if(*(uint*)ip == (*(uint*)IP | ~*(uint*)Mask)) return true;
	if(ip == (IP | ~Mask)) return true;

	return false;
}
#endif

#define TinyIP_DHCP
#ifdef TinyIP_DHCP
void Dhcp::SendDhcp(DHCP_HEADER* dhcp, uint len)
{
	byte* p = dhcp->Next();
	if(p[len - 1] != DHCP_OPT_End)
	{
		// 此时指向的是负载数据后的第一个字节，所以第一个opt不许Next
		DHCP_OPT* opt = (DHCP_OPT*)(p + len);
		opt = opt->SetClientId(Tip->Mac, 6);
		opt = opt->Next()->SetData(DHCP_OPT_RequestedIP, Tip->IP);
		opt = opt->Next()->SetData(DHCP_OPT_HostName, (byte*)"YWS_SmartOS", 11);
		opt = opt->Next()->SetData(DHCP_OPT_Vendor, (byte*)"http://www.NewLifeX.com", 23);
		byte ps[] = { 0x01, 0x06, 0x03, 0x2b}; // 需要参数 Mask/DNS/Router/Vendor
		opt = opt->Next()->SetData(DHCP_OPT_ParameterList, ps, ArrayLength(ps));
		opt = opt->Next()->End();

		len = (byte*)opt + 1 - p;
	}

	memcpy(dhcp->ClientMac, Tip->Mac, 6);

	memset(Tip->RemoteMac, 0xFF, 6);
	//memset(IP, 0x00, 4);
	//memset(Tip->RemoteIP, 0xFF, 4);
	Tip->RemoteIP = 0xFFFFFFFF;
	Tip->Port = 68;
	Tip->RemotePort = 67;

	// 如果最后一个字节不是DHCP_OPT_End，则增加End
	//byte* p = (byte*)dhcp + sizeof(DHCP_HEADER);
	//if(p[len - 1] != DHCP_OPT_End) p[len++] = DHCP_OPT_End;

	Send((byte*)dhcp, sizeof(DHCP_HEADER) + len, false);
}

// 获取选项，返回数据部分指针
DHCP_OPT* GetOption(byte* p, int len, DHCP_OPTION option)
{
	byte* end = p + len;
	while(p < end)
	{
		byte opt = *p++;
		byte len = *p++;
		if(opt == DHCP_OPT_End) return 0;
		if(opt == option) return (DHCP_OPT*)(p - 2);

		p += len;
	}

	return 0;
}

// 设置选项
void SetOption(byte* p, int len)
{
}

// 找服务器
void Dhcp::Discover(DHCP_HEADER* dhcp)
{
	byte* p = dhcp->Next();
	DHCP_OPT* opt = (DHCP_OPT*)p;
	opt->SetType(DHCP_TYPE_Discover);

	SendDhcp(dhcp, (byte*)opt->Next() - p);
}

void Dhcp::Request(DHCP_HEADER* dhcp)
{
	byte* p = dhcp->Next();
	DHCP_OPT* opt = (DHCP_OPT*)p;
	opt->SetType(DHCP_TYPE_Request);
	opt = opt->Next()->SetData(DHCP_OPT_DHCPServer, Tip->DHCPServer);

	SendDhcp(dhcp, (byte*)opt->Next() - p);
}

void Dhcp::PareOption(byte* buf, uint len)
{
	byte* p = buf;
	byte* end = p + len;
	while(p < end)
	{
		byte opt = *p++;
		if(opt == DHCP_OPT_End) break;
		byte len = *p++;
		// 有些家用路由器会发送错误的len，大于4字节，导致覆盖前后数据
		switch(opt)
		{
			case DHCP_OPT_Mask: Tip->Mask = *(uint*)p; break;
			case DHCP_OPT_DNSServer: Tip->DNSServer = *(uint*)p; break;
			case DHCP_OPT_Router: Tip->Gateway = *(uint*)p; break;
			case DHCP_OPT_DHCPServer: Tip->DHCPServer = *(uint*)p; break;
#if NET_DEBUG
			//default:
			//	debug_printf("Unkown DHCP Option=%d Length=%d\r\n", opt, len);
#endif
		}
		p += len;
	}
}

void RenewDHCP(void* param)
{
	TinyIP* tip = (TinyIP*)param;
	if(tip)
	{
		Dhcp dhcp(tip);
		if(!dhcp.Start())
		{
			debug_printf("TinyIP DHCP Fail!\r\n\r\n");
			return;
		}
	}
}

bool Dhcp::Start()
{
	uint dhcpid = (uint)Time.CurrentTicks();

	byte buf[400];
	uint bufSize = ArrayLength(buf);

	ETH_HEADER*  eth  = (ETH_HEADER*) buf;
	IP_HEADER*   ip   = (IP_HEADER*)  eth->Next();
	UDP_HEADER*  udp  = (UDP_HEADER*) ip->Next();
	DHCP_HEADER* dhcp = (DHCP_HEADER*)udp->Next();

	//ulong next = 0;
	// 总等待时间
	//ulong end = Time.Current() + 10 * 1000000;
	//while(end > Time.Current())
	TimeWheel tw(10);
	TimeWheel next(0);
	while(!tw.Expired())
	{
		// 得不到就重新发广播
		//if(next < Time.Current())
		if(next.Expired())
		{
			// 向DHCP服务器广播
			debug_printf("DHCP Discover...\r\n");
			dhcp->Init(dhcpid, true);
			Discover(dhcp);

			//next = Time.Current() + 1 * 1000000;
			next.Reset(1);
		}

		uint len = Tip->Fetch(buf, bufSize);
        // 如果缓冲器里面没有数据则转入下一次循环
		if(!len)
		{
			Sys.Sleep(2);	// 等待一段时间，释放CPU

			continue;
		}

		if(eth->Type != ETH_IP || ip->Protocol != IP_UDP) continue;

		if(__REV16(udp->DestPort) != 68) continue;

		// DHCP附加数据的长度
		//len -= dhcp->Offset() + dhcp->Size();
		len -= dhcp->Next() - buf;
		if(len <= 0) continue;

		if(!dhcp->Valid())continue;

		byte* data = dhcp->Next();

		// 获取DHCP消息类型
		DHCP_OPT* opt = GetOption(data, len, DHCP_OPT_MessageType);
		if(!opt) continue;

		if(opt->Data == DHCP_TYPE_Offer)
		{
			if(__REV(dhcp->TransID) == dhcpid)
			{
				//memcpy(Tip->IP, dhcp->YourIP, 4);
				Tip->IP = dhcp->YourIP;
				PareOption(dhcp->Next(), len);

				// 向网络宣告已经确认使用哪一个DHCP服务器提供的IP地址
				// 这里其实还应该发送ARP包确认IP是否被占用，如果被占用，还需要拒绝服务器提供的IP，比较复杂，可能性很低，暂时不考虑
#if NET_DEBUG
				debug_printf("DHCP Offer IP:");
				TinyIP::ShowIP(Tip->IP);
				debug_printf(" From ");
				TinyIP::ShowIP(ip->SrcIP);
				debug_printf("\r\n");
#endif

				dhcp->Init(dhcpid, true);
				Request(dhcp);
			}
		}
		else if(opt->Data == DHCP_TYPE_Ack)
		{
#if NET_DEBUG
			debug_printf("DHCP Ack   IP:");
			TinyIP::ShowIP(dhcp->YourIP);
			debug_printf(" From ");
			TinyIP::ShowIP(ip->SrcIP);
			debug_printf("\r\n");
#endif

			//if(memcmp(dhcp->YourIP, Tip->IP, 4) == 0)
			if(dhcp->YourIP == Tip->IP)
			{
				//IPIsReady = true;

				// 查找租约时间，提前续约
				opt = GetOption(data, len, DHCP_OPT_IPLeaseTime);
				if(opt)
				{
					// 续约时间，大字节序，时间单位秒
					uint time = __REV(*(uint*)&opt->Data);
					// DHCP租约过了一半以后重新获取IP地址
					if(time > 0) Sys.AddTask(RenewDHCP, Tip, time / 2 * 1000000, -1);
				}

				return true;
			}
		}
#if NET_DEBUG
		else if(opt->Data == DHCP_TYPE_Nak)
		{
			// 导致Nak的原因
			opt = GetOption(data, len, DHCP_OPT_Message);
			debug_printf("DHCP Nak   IP:");
			TinyIP::ShowIP(Tip->IP);
			debug_printf(" From ");
			TinyIP::ShowIP(ip->SrcIP);
			if(opt)
			{
				debug_printf(" ");
				Sys.ShowString(&opt->Data, opt->Length);
			}
			debug_printf("\r\n");
		}
		else
			debug_printf("DHCP Unkown Type=%d\r\n", opt->Data);
#endif
	}

	return false;
}
#endif

Socket::Socket(TinyIP* tip)
{
	assert_param(tip);

	Tip = tip;
	// 加入到列表
	tip->Sockets.Add(this);
	//__packed List<Socket*>* list = &tip->Sockets;
	//list->Add(this);
}

Socket::~Socket()
{
	assert_param(Tip);

	// 从TinyIP中删除当前Socket
	Tip->Sockets.Remove(this);
}

ArpSocket::ArpSocket(TinyIP* tip) : Socket(tip)
{
	Type = ETH_ARP;

#ifdef STM32F0
	Count = 4;
#elif defined(STM32F1)
	Count = 16;
#elif defined(STM32F4)
	Count = 64;
#endif
	_Arps = NULL;
}

ArpSocket::~ArpSocket()
{
	if(_Arps) delete _Arps;
	_Arps = NULL;
}

bool ArpSocket::Process(MemoryStream* ms)
{
	// 前面的数据长度很不靠谱，这里进行小范围修正
	//uint size = ms->Position + sizeof(ARP_HEADER);
	//if(ms->Length < size) ms->Length = size;

	ARP_HEADER* arp = ms->Retrieve<ARP_HEADER>();
	if(!arp) return false;

	/*
	当封装的ARP报文在以太网上传输时，硬件类型字段赋值为0x0100，标识硬件为以太网硬件;
	协议类型字段赋值为0x0800，标识上次协议为IP协议;
	由于以太网的MAC地址为48比特位，IP地址为32比特位，则硬件地址长度字段赋值为6，协议地址长度字段赋值为4;
	选项字段标识ARP报文的类型，当为请求报文时，赋值为0x0100，当为回答报文时，赋值为0x0200。
	*/

	// 如果是Arp响应包，自动加入缓存
	/*if(arp->Option == 0x0200)
	{
		AddArp(arp->SrcIP, arp->SrcMac);
	}
	// 别人的响应包这里收不到呀，还是把请求包也算上吧
	if(arp->Option == 0x0100)
	{
		AddArp(arp->SrcIP, arp->SrcMac);
	}*/

	// 是否发给本机。注意memcmp相等返回0
	//if(memcmp(arp->DestIP, Tip->IP, 4) != 0) return true;
	//if(*(uint*)arp->DestIP != *(uint*)Tip->IP) return true;
	if(arp->DestIP != Tip->IP) return true;

#if NET_DEBUG
	// 数据校验
	assert_param(arp->HardType == 0x0100);
	assert_param(arp->ProtocolType == ETH_IP);
	assert_param(arp->HardLength == 6);
	assert_param(arp->ProtocolLength == 4);
	assert_param(arp->Option == 0x0100);

	if(arp->Option == 0x0100)
		debug_printf("ARP Request For ");
	else
		debug_printf("ARP Response For ");

	Tip->ShowIP(arp->DestIP);
	debug_printf(" <= ");
	Tip->ShowIP(arp->SrcIP);
	debug_printf(" [");
	Tip->ShowMac(arp->SrcMac);
	debug_printf("] Payload=%d\r\n", ms->Remain());
#endif

	// 构造响应包
	arp->Option = 0x0200;
	// 来源IP和Mac作为目的地址
	memcpy(arp->DestMac, arp->SrcMac, 6);
	//memcpy(arp->DestIP, arp->SrcIP, 4);
	memcpy(arp->SrcMac, Tip->Mac, 6);
	//memcpy(arp->SrcIP, Tip->IP, 4);
	arp->DestIP = arp->SrcIP;
	arp->SrcIP = Tip->IP;

#if NET_DEBUG
	debug_printf("ARP Response To ");
	Tip->ShowIP(arp->DestIP);
	debug_printf(" size=%d\r\n", sizeof(ARP_HEADER));
#endif

	Tip->SendEthernet(ETH_ARP, (byte*)arp, sizeof(ARP_HEADER));

	return true;
}

// 请求Arp并返回其Mac。timeout超时3秒，如果没有超时时间，表示异步请求，不用等待结果
const byte* ArpSocket::Request(IPAddress ip, int timeout)
{
	// 缓冲区必须略大，否则接收数据时可能少一个字节
	byte buf[sizeof(ETH_HEADER) + sizeof(ARP_HEADER) + 4];
	uint bufSize = ArrayLength(buf);
	MemoryStream ms(buf, bufSize);

	ETH_HEADER* eth = (ETH_HEADER*)buf;
	ARP_HEADER* arp = (ARP_HEADER*)eth->Next();
	arp->Init();

	// 构造请求包
	arp->Option = 0x0100;
	memcpy(arp->DestMac, g_ZeroMac, 6);
	//memcpy(arp->DestIP, ip, 4);
	memcpy(arp->SrcMac, Tip->Mac, 6);
	//memcpy(arp->SrcIP, Tip->IP, 4);
	memcpy(Tip->RemoteMac, g_ZeroMac, 6);
	arp->DestIP = ip;
	arp->SrcIP = Tip->IP;

#if NET_DEBUG
	debug_printf("ARP Request To ");
	Tip->ShowIP(arp->DestIP);
	debug_printf(" size=%d\r\n", sizeof(ARP_HEADER));
#endif

	Tip->SendEthernet(ETH_ARP, (byte*)arp, sizeof(ARP_HEADER));

	// 如果没有超时时间，表示异步请求，不用等待结果
	if(timeout <= 0) return NULL;

	// 总等待时间
	//ulong end = Time.Current() + timeout * 1000000;
	//while(end > Time.Current())
	TimeWheel tw(1, 0, 0);
	while(!tw.Expired())
	{
		// 阻塞其它任务，频繁调度OnWork，等待目标数据
		uint len = Tip->Fetch(buf, bufSize);
		if(!len)
		{
			Sys.Sleep(2);	// 等待一段时间，释放CPU

			continue;
		}

		// 处理ARP
		if(eth->Type == ETH_ARP)
		{
			// 是否目标发给本机的Arp响应包。注意memcmp相等返回0
			//if(memcmp(arp->DestIP, Tip->IP, 4) == 0
			if(arp->DestIP == Tip->IP
			// 不要那么严格，只要有源MAC地址，即使不是发给本机，也可以使用
			//&& memcmp(arp->SrcIP, ip, 4) == 0
			&& arp->Option == 0x0200)
			{
				return arp->SrcMac;
			}
		}

		// 用不到数据包交由系统处理
		//Tip->Process(buf, bufSize, 0, len);
		ms.Position = 0;
		ms.Length = len;
		Tip->Process(&ms);
	}

	return NULL;
}

const byte* ArpSocket::Resolve(IPAddress ip)
{
	if(Tip->IsBroadcast(ip)) return g_FullMac;

	// 如果不在本子网，那么应该找网关的Mac
	/*uint mask = *(uint*)Tip->Mask;
	if((*(uint*)ip & mask) != (*(uint*)Tip->IP & mask)) ip = Tip->Gateway;*/
	if((ip & Tip->Mask) != (Tip->IP & Tip->Mask)) ip = Tip->Gateway;
	/*if(ip[0] & Tip->Mask[0] != Tip->IP[0] & Tip->Mask[0]
	|| ip[1] & Tip->Mask[1] != Tip->IP[1] & Tip->Mask[1]
	|| ip[2] & Tip->Mask[2] != Tip->IP[2] & Tip->Mask[2]
	|| ip[3] & Tip->Mask[3] != Tip->IP[3] & Tip->Mask[3]
	) ip = Tip->Gateway;*/
	// 下面的也可以，但是比较难理解
	/*if(ip[0] ^ IP[0] != ~Mask[0]
	|| ip[1] ^ IP[1] != ~Mask[1]
	|| ip[2] ^ IP[2] != ~Mask[2]
	|| ip[3] ^ IP[3] != ~Mask[3]
	) ip = Gateway;*/

	ARP_ITEM* item = NULL;	// 匹配项
	if(_Arps)
	{
		//uint sNow = Time.Current() / 1000000;	// 当前时间，秒
		uint sNow = Time.Current() >> 20;	// 当前时间，秒
		// 在表中查找
		for(int i=0; i<Count; i++)
		{
			ARP_ITEM* arp = &_Arps[i];
			//if(memcmp(arp->IP, ip, 4) == 0)
			if(arp->IP == ip)
			{
				// 如果未过期，则直接使用。否则重新请求
				if(arp->Time > sNow) return arp->Mac;

				// 暂时保存，待会可能请求失败，还可以用旧的顶着
				item = arp;
			}
		}
	}

	// 找不到则发送Arp请求。如果有旧值，则使用异步请求即可
	const byte* mac = Request(ip, item ? 0 : 3);
	if(!mac) return item ? item->Mac : NULL;

	Add(ip, mac);

	return mac;
}

void ArpSocket::Add(IPAddress ip, const byte mac[6])
{
#if NET_DEBUG
	debug_printf("Add Arp(");
	TinyIP::ShowIP(ip);
	debug_printf(", ");
	TinyIP::ShowMac(mac);
	debug_printf(")\r\n");
#endif

	if(!_Arps)
	{
		_Arps = new ARP_ITEM[Count];
		memset(_Arps, 0, sizeof(ARP_ITEM) * Count);
	}

	ARP_ITEM* item = NULL;
	ARP_ITEM* empty = NULL;
	// 在表中查找项
	//const byte ipnull[] = { 0, 0, 0, 0 };
	for(int i=0; i<Count; i++)
	{
		ARP_ITEM* arp = &_Arps[i];
		//if(memcmp(arp->IP, ip, 4) == 0)
		if(arp->IP == ip)
		{
			item = arp;
			break;
		}
		//if(!empty && memcmp(arp->IP, ipnull, 4) == 0)
		if(!empty && arp->IP == 0)
		{
			empty = arp;
			break;
		}
	}

	// 如果没有匹配项，则使用空项
	if(!item) item = empty;
	// 如果也没有空项，表示满了，那就替换最老的一个
	if(!item)
	{
		uint oldTime = 0xFFFFFFFF;
		// 在表中查找最老项用于替换
		for(int i=0; i<Count; i++)
		{
			ARP_ITEM* arp = &_Arps[i];
			// 找最老的一个，待会如果需要覆盖，就先覆盖它。避开网关
			//if(arp->Time < oldTime && memcmp(arp->IP, Tip->Gateway, 4) != 0)
			if(arp->Time < oldTime && arp->IP != Tip->Gateway)
			{
				oldTime = arp->Time;
				item = arp;
			}
		}
#if NET_DEBUG
		debug_printf("Arp Table is full, replace ");
		TinyIP::ShowIP(item->IP);
		debug_printf("\r\n");
#endif
	}

	//uint sNow = Time.Current() / 1000000;	// 当前时间，秒
	uint sNow = Time.Current() >> 20;	// 当前时间，秒
	// 保存
	//memcpy(item->IP, ip, 4);
	item->IP = ip;
	memcpy(item->Mac, mac, 6);
	item->Time = sNow + 60;	// 默认过期时间1分钟
}
