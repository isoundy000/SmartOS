﻿#include "AP0801.h"

#include "SerialPort.h"
#include "WatchDog.h"
#include "Config.h"

#include "Drivers\NRF24L01.h"
#include "Drivers\W5500.h"
#include "Drivers\Esp8266.h"

#include "Net\Dhcp.h"
#include "Net\DNS.h"

#include "TokenNet\TokenController.h"

#include "App\FlushPort.h"

static FlushPort* CreateFlushPort(OutputPort* led)
{
	auto fp	= new FlushPort();
	fp->Port	= led;
	fp->Start();

	return fp;
}

AP0801::AP0801()
{
	EthernetLed	= nullptr;
	WirelessLed	= nullptr;
}

#if DEBUG
static uint OnSerial(ITransport* transport, Buffer& bs, void* param, void* param2)
{
	debug_printf("OnSerial len=%d \t", bs.Length());
	bs.Show(true);

	return 0;
}
#endif

void AP0801::Setup(ushort code, const char* name, COM message, int baudRate)
{
	auto& sys	= (TSys&)Sys;
	sys.Code = code;
	sys.Name = (char*)name;

    // 初始化系统
    //Sys.Clock = 48000000;
    sys.Init();
#if DEBUG
    sys.MessagePort = message; // 指定printf输出的串口
    Sys.ShowInfo();
#endif

#if DEBUG
	// 设置一定量初始任务，减少堆分配
	static Task ts[0x10];
	Task::Scheduler()->Set(ts, ArrayLength(ts));
#endif

#if DEBUG
	// 打开串口输入便于调试数据操作，但是会影响性能
	if(baudRate > 0)
	{
		auto sp = SerialPort::GetMessagePort();
		if(baudRate != 1024000)
		{
			sp->Close();
			sp->SetBaudRate(baudRate);
		}
		sp->Register(OnSerial);
	}

	//WatchDog::Start(20000);
#else
	WatchDog::Start();
#endif

	// Flash最后一块作为配置区
	Config::Current	= &Config::CreateFlash();
}

ISocketHost* AP0801::Create5500()
{
	debug_printf("\r\nW5500::Create \r\n");

	auto spi	= new Spi(Spi2, 36000000);

	auto net	= new W5500();
	net->LoadConfig();
	net->Init(spi, PE1, PD13);

	if(EthernetLed) net->Led	= CreateFlushPort(EthernetLed);

	Host	= net;

	return net;
}

ISocketHost* AP0801::Create8266(Action onNetReady)
{
	debug_printf("\r\nEsp8266::Create \r\n");

	/*// 上电
	auto pwr	= new OutputPort(PE2);
	pwr->Down(1000);*/

	auto srp	= new SerialPort(COM4, 115200);
	srp->ByteTime	= 10;
	srp->Tx.SetCapacity(0x400);
	srp->Rx.SetCapacity(0x400);

	auto net	= new Esp8266(srp, PE2, PD3);
	net->InitConfig();
	net->LoadConfig();

	net->SSID	= "yws007";
	net->Pass	= "yws52718";

	if(EthernetLed) net->Led	= CreateFlushPort(EthernetLed);
	net->NetReady	= onNetReady;

	net->OpenAsync();

	Host	= net;

	return net;
}

/******************************** DHCP ********************************/

static Action _DHCP_Ready = nullptr;

static void On_DHCP_Ready(void* param)
{
	if(_DHCP_Ready) _DHCP_Ready(param);
}

static void OnDhcpStop(void* sender, void* param)
{
	auto& dhcp = *(Dhcp*)sender;

	// DHCP成功，或者失败且超过最大错误次数，都要启动网关，让它以上一次配置工作
	if(dhcp.Result || dhcp.Times >= dhcp.MaxTimes)
	{
		// 防止调用栈太深，另外开任务
		if(_DHCP_Ready) Sys.AddTask(On_DHCP_Ready, &dhcp.Host, 0, -1, "网络就绪");
	}
}

void AP0801::InitDHCP(Action onNetReady)
{
	_DHCP_Ready	= onNetReady;

	// 打开DHCP
	auto dhcp	= new Dhcp(*Host);
	dhcp->OnStop	= OnDhcpStop;
	dhcp->Start();
}

/******************************** DNS ********************************/

static IPAddress QueryDNS(ISocketHost* host, const String& domain)
{
	return DNS::Query(*host, domain);
}

void AP0801::InitDNS()
{
	// 只有W5500需要DNS支持
	auto net	= dynamic_cast<W5500*>(Host);
	if(!net) return;

	net->OnResolve	= QueryDNS;
}

/*bool AP0801::QueryDNS(TokenConfig& tk)
{
	if(tk.Server.Length() == 0) return false;

	// 根据DNS获取云端IP地址
	auto ip	= DNS::Query(*Host, tk.Server);
	if(ip == IPAddress::Any())
	{
		debug_printf("DNS::Query %s 失败！\r\n", tk.Server.GetBuffer());
		return false;
	}
	debug_printf("服务器地址 %s %s:%d \r\n", tk.Server.GetBuffer(), ip.ToString().GetBuffer(), tk.ServerPort);

	tk.ServerIP = ip.Value;
	tk.Save();

	return true;
}*/

/******************************** Token ********************************/

TokenClient* AP0801::CreateClient()
{
	debug_printf("\r\nCreateClient \r\n");

	auto tk = TokenConfig::Current;

	// 创建连接服务器的Socket
	auto socket	= Host->CreateSocket(tk->Protocol);
	socket->Remote.Port		= tk->ServerPort;
	socket->Remote.Address	= IPAddress(tk->ServerIP);
	socket->Server	= tk->Server;

	// 创建连接服务器的控制器
	auto ctrl	= new TokenController();
	ctrl->Port = dynamic_cast<ITransport*>(socket);

	// 创建客户端
	auto client	= new TokenClient();
	client->Control	= ctrl;
	//client->Local	= ctrl;
	client->Cfg		= tk;

	// 如果是TCP，需要再建立一个本地UDP
	//if(tk->Protocol == ProtocolType::Tcp)
	{
		// 建立一个监听内网的UDP Socket
		socket	= Host->CreateSocket(ProtocolType::Udp);
		socket->Remote.Port		= 3355;	// 广播端口。其实用哪一个都不重要，因为不会主动广播
		socket->Remote.Address	= IPAddress::Broadcast();
		socket->Local.Port	= tk->Port;

		// 建立内网控制器
		auto token2		= new TokenController();
		token2->Port	= dynamic_cast<ITransport*>(socket);
		client->Local	= token2;
	}

	return client;
}

/******************************** 2401 ********************************/

/*int Fix2401(const Buffer& bs)
{
	//auto& bs	= *(Buffer*)param;
	// 微网指令特殊处理长度
	uint rs	= bs.Length();
	if(rs >= 8)
	{
		rs = bs[5] + 8;
		//if(rs < bs.Length()) bs.SetLength(rs);
	}
	return rs;
}

ITransport* AP0801::Create2401(SPI spi_, Pin ce, Pin irq, Pin power, bool powerInvert, IDataPort* led)
{
	debug_printf("\r\n Create2401 \r\n");

	static Spi spi(spi_, 10000000, true);
	static NRF24L01 nrf;
	nrf.Init(&spi, ce, irq, power);

	auto tc	= TinyConfig::Create();
	if(tc->Channel == 0)
	{
		tc->Channel	= 120;
		tc->Speed	= 250;
	}
	if(tc->Interval == 0)
	{
		tc->Interval= 40;
		tc->Timeout	= 1000;
	}

	nrf.AutoAnswer	= false;
	nrf.DynPayload	= false;
	nrf.Channel		= tc->Channel;
	//nrf.Channel		= 120;
	nrf.Speed		= tc->Speed;

	nrf.FixData	= Fix2401;

	if(WirelessLed) net->Led	= CreateFlushPort(WirelessLed);

	nrf.Master	= true;

	return &nrf;
}*/

/*
NRF24L01+ 	(SPI3)		|	W5500		(SPI2)		|	TOUCH		(SPI3)
NSS			|				NSS			|	PD6			NSS
CLK			|				SCK			|				SCK
MISO		|				MISO		|				MISO
MOSI		|				MOSI		|				MOSI
PE3			IRQ			|	PE1			INT(IRQ)	|	PD11		INT(IRQ)
PD12		CE			|	PD13		NET_NRST	|				NET_NRST
PE6			POWER		|				POWER		|				POWER

ESP8266		(COM4)
TX
RX
PD3			RST
PE2			POWER

TFT
FSMC_D 0..15		TFT_D 0..15
NOE					RD
NWE					RW
NE1					RS
PE4					CE
PC7					LIGHT
PE5					RST

PE13				KEY1
PE14				KEY2

PE15				LED2
PD8					LED1


USB
PA11 PA12
*/