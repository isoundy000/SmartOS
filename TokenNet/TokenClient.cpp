﻿#include "Time.h"

#include "Net\Net.h"
#include "TokenClient.h"

#include "TokenMessage.h"
#include "HelloMessage.h"
#include "LoginMessage.h"
#include "RegisterMessage.h"

static bool OnTokenClientReceived(void* sender, Message& msg, void* param);

static void LoopTask(void* param);

TokenClient::TokenClient() : ID(16), Key(16)
{
	Token		= 0;
	// 直接拷贝芯片ID和类型作为唯一ID
	ID.Set(Sys.ID, ID.Length());
	Key.Set(Sys.ID, Key.Length());

	Status		= 0;
	LoginTime	= 0;
	LastActive	= 0;
	Delay		= 0;

	IsOldOrder	= false;	//是否旧指令

	Control		= NULL;

	Received	= NULL;
	Param		= NULL;
	TokenConfig	= NULL;

	Local		= NULL;
}

void TokenClient::Open()
{
	assert_param2(Control, "令牌客户端还没设置控制器呢");

	Control->Received	= OnTokenClientReceived;
	Control->Param		= this;
	Control->Open();

	if(Local && Local != Control)
	{
		Local->Received	= OnTokenClientReceived;
		Local->Param	= this;
		Local->Open();
	}
	
	TokenConfig	= TokenConfig::Current;
		
	// 设置握手广播的本地地址和端口
	//ITransport* port = Control->Port;
	// C++的多接口跟C#不一样，不能简单转换了事，还需要注意两个接口的先后顺序，让它偏移
	//ISocket* sock = (ISocket*)(port + 1);
	ISocket* sock = dynamic_cast<ISocket*>(Control->Port);
	if(sock) Hello.EndPoint = sock->Local;

	// 令牌客户端定时任务
	_task = Sys.AddTask(LoopTask, this, 1000, 5000, "令牌客户端");
}

void TokenClient::Close()
{
	Sys.RemoveTask(_task);
}

bool TokenClient::Send(TokenMessage& msg, Controller* ctrl)
{
	if(!ctrl) ctrl	= Control;
	assert_param2(ctrl, "TokenClient::Send");

	return ctrl->Send(msg);
}

bool TokenClient::Reply(TokenMessage& msg, Controller* ctrl)
{
	//if(Status < 2) return false;

	if(!ctrl) ctrl	= Control;
	assert_param2(ctrl, "TokenClient::Reply");

	return ctrl->Reply(msg);
}

bool TokenClient::OnReceive(TokenMessage& msg, Controller* ctrl)
{
	LastActive = Sys.Ms();

	switch(msg.Code)
	{
		case 0x01:
			OnHello(msg, ctrl);
			break;
		case 0x02:
			if(msg.Reply)
				OnLogin(msg, ctrl);
			else	
				Login(msg, ctrl);
			break;
		case 0x03:
			OnPing(msg, ctrl);
			break;
		case 0x07:
			OnRegister(msg, ctrl);
			break;
	}

	// 消息转发
	if(Received) Received(ctrl, msg, Param);

	return true;
}

bool OnTokenClientReceived(void* sender, Message& msg, void* param)
{
	auto ctrl = (Controller*)sender;
	assert_ptr(ctrl);
	auto client = (TokenClient*)param;
	assert_ptr(client);

	return client->OnReceive((TokenMessage&)msg, ctrl);
}

// 常用系统级消息

// 定时任务
void LoopTask(void* param)
{	
	assert_ptr(param);
	TokenClient* client = (TokenClient*)param;
	//client->SayHello(false);
	//if(client->Udp->BindPort != 3355)
	//	client->SayHello(true, 3355);

	// 状态。0准备、1握手完成、2登录后
	switch(client->Status)
	{
		case 0:
			client->SayHello(false);
			break;
		case 1:
			client->Login();
			break;
		case 2:
			client->Ping();
			break;
		case 3:
			client->Register();
			break;
	}
}

// 发送发现消息，告诉大家我在这
// 请求：2版本 + S类型 + S名称 + 8本地时间 + 本地IP端口 + N支持加密算法列表
// 响应：2版本 + S类型 + S名称 + 8对方时间 + 对方IP端口 + 1加密算法 + N密钥
void TokenClient::SayHello(bool broadcast, int port)
{
	TokenMessage msg(0x01);

	HelloMessage ext(Hello);
	ext.Reply		= false;
	ext.LocalTime	= Time.Now().TotalMicroseconds();
	ext.WriteMessage(msg);
	ext.Show(true);

	// 广播消息直接用UDP发出
	/*if(broadcast)
	{
		if(!Udp) return;

		// 临时修改远程地址为广播地址
		IPEndPoint ep = Udp->Remote;
		Udp->Remote.Address	= IPAddress::Broadcast;
		Udp->Remote.Port	= port;

		debug_printf("握手广播 ");
		Control->Send(msg);

		// 还原回来原来的地址端口
		Udp->Remote = ep;

		return;
	}*/

	Send(msg);
}

// 握手响应
bool TokenClient::OnHello(TokenMessage& msg, Controller* ctrl)
{
	// 如果收到响应，并且来自来源服务器
	if(msg.Reply)
	{
		if(msg.Error)
		{
			if(HelloRedirect(msg))
				return false;
			
			Stream ms = msg.ToStream();
			byte err  = ms.ReadByte();
			
			Status	= 0;
			Token	= 0;
			debug_printf("握手失败，错误码=0x%02X ",err);
			
			char cs[0x100];
			String str(cs, ArrayLength(cs));
			ms.ReadArray(str);
			str.Show(true);
		}
		else
		{
			debug_printf("握手完成，开始登录……\r\n");
			// 解析数据
	        HelloMessage ext;
	        ext.Reply = msg.Reply;

	        ext.ReadMessage(msg);
	        ext.Show(true);

			// 通讯密码
			if(ext.Key.Length() > 0)
			{
				auto ctrl2	= dynamic_cast<TokenController*>(ctrl);
				if(ctrl2) ctrl2->Key = ext.Key;

				debug_printf("握手得到通信密码：");
				ext.Key.Show(true);
				
				if(TokenConfig->New)					
					Status = 3;
			    else
					Status = 1;	
			}
			
			if(ext.Version == 0x00) Token = 0;

			// 同步本地时间
			if(ext.LocalTime > 0) Time.SetTime(ext.LocalTime / 1000000UL);
			if(Status == 1)
				Login();
		}
	}
	else if(!msg.Reply)
	{
		TokenMessage rs;
		rs.Code		= msg.Code;

		HelloMessage ext2(Hello);
		ext2.Reply	= true;
		// 使用系统ID作为Name
		//ext2.Name.Copy(Sys.ID, 16);
		// 使用系统ID作为Key		
		ext2.Key.Copy(Sys.ID, 16);
		auto ctrl3	= dynamic_cast<TokenController*>(ctrl);
		if(ctrl3) ctrl3->Key = ext2.Key;
		
		ext2.Ciphers[0]	= 0xFF;
		//ext2.LocalTime = ext.LocalTime;
		// 使用当前时间
		ext2.LocalTime = Sys.Ms() * 1000;
		ext2.WriteMessage(rs);

		// 源地址发回去
		rs.State	= msg.State;

		Reply(rs);
	}

	return true;
}

bool TokenClient::HelloRedirect(TokenMessage& msg)
{
    // 解析数据
	Stream ms(msg.Data, msg.Length);

	if(ms.ReadByte() < 0xFE) return false;

	auto cfg	= TokenConfig;
	cfg->Protocol	= ms.ReadByte();
	
	uint len = ms.ReadByte();

	if(len > ArrayLength(cfg-> Server)) len = ArrayLength(cfg-> Server);
	for(int i=0;i!=len;i++)
	{
		cfg-> Server[i]=ms.ReadByte();
    }
	
	cfg->ServerPort = ms.ReadUInt16();
	debug_printf("cfg->ServerPort:%d\r\n",cfg->ServerPort);
	strcpy(cfg->Vendor, "s1.peacemoon.cn");

	cfg->Save();
    cfg->Show();

	Sys.Reset();

	return true;
}
//注册
void TokenClient::Register()
{
	debug_printf("TokenClient::Register\r\n");
	RegisterMessage re;	
	re.Name = ID;
	re.Pass	= Key;
	
	TokenMessage msg(7);
	re.WriteMessage(msg);
	Send(msg);	
	
}
void TokenClient::OnRegister(TokenMessage& msg ,Controller* ctrl)
{
	Stream ms(msg.Data, msg.Length);	
	auto cfg	= TokenConfig;
	
	uint namelen = ms.ReadByte();
	if(namelen > 16) return;
	
	for(int i=0;i!=namelen;i++)
	{
		cfg-> Name[i]=ms.ReadByte();
    }
	
	uint passlen = ms.ReadByte();
	if(passlen > 16) return;
	
	for(int i=0;i!=passlen;i++)
	{
		cfg->Key[i]=ms.ReadByte();
    }
	
	cfg->Save();
    cfg->Show();

	Sys.Reset();		
}
// 登录
void TokenClient::Login()
{
	LoginMessage login;
	login.Name	= TokenConfig->Name;
	login.Key	= TokenConfig->Key;

	TokenMessage msg(2);
	login.WriteMessage(msg);

	Send(msg);
}

void TokenClient::Login(TokenMessage& msg,Controller* ctrl)
{
	if(msg.Error) return;
	
	LoginMessage login;
	login.Key		= TokenConfig->Name;
	login.Token		= 1;
	login.Reply		= true;	
	login.WriteMessage(msg);
		
	Reply(msg);	

	auto ctrl2		= dynamic_cast<TokenController*>(ctrl);	
	ctrl2->Key		= login.Key;
	ctrl2->Token 	= login.Token;	
		 
}

bool TokenClient::OnLogin(TokenMessage& msg, Controller* ctrl)
{
	if(!msg.Reply) return;	
	Stream ms(msg.Data, msg.Length);

	if(msg.Error)
	{
		// 登录失败，清空令牌
		Token = 0;

		byte result = ms.ReadByte();
		//if(result == 0xFF) Status = 0;
		// 任何错误，重新握手
		Status = 0;

		debug_printf("登录失败，错误码 0x%02X！", result);
		ms.ReadString().Show(true);
	}
	else
	{
		Status = 2;
		debug_printf("登录成功！ ");

		if(IsOldOrder)
		{
			byte stat=ms.ReadByte();//旧指令，读走状态码
		}
		// 得到令牌
		Token = ms.ReadUInt32();
		debug_printf("令牌：0x%08X ", Token);
		// 这里可能有通信密码
		if(ms.Remain() > 0)
		{
			auto bs = ms.ReadArray();
			if(bs.Length() > 0)
			{
				auto ctrl2	= dynamic_cast<TokenController*>(ctrl);
				if(ctrl2) ctrl2->Key = bs;

				debug_printf("通信密码：");
				bs.Show();
			}
		}
		debug_printf("\r\n");
	}

	return true;
}

// Ping指令用于保持与对方的活动状态
void TokenClient::Ping()
{
	if(LastActive > 0 && LastActive + 30000 < Sys.Ms())
	{
		// 30秒无法联系，服务端可能已经掉线，重启Hello任务
		debug_printf("30秒无法联系，服务端可能已经掉线，重新开始握手\r\n");

		Status = 0;

		return;
	}

	TokenMessage msg(3);

	ulong time	= Sys.Ms();
	Stream ms	= msg.ToStream();
	ms.WriteArray(Array(&time, 8));
	msg.Length	= ms.Position();

	Send(msg);
}

bool TokenClient::OnPing(TokenMessage& msg, Controller* ctrl)
{
	// 忽略
	if(!msg.Reply) return Reply(msg);

	Stream ms = msg.ToStream();

	ulong now = Sys.Ms();
	ulong start = ms.ReadArray().ToUInt64();
	int cost = (int)(now - start);
	if(cost < 0) cost = -cost;
	if(cost > 1000) Time.SetTime(start / 1000);

	if(Delay)
		Delay = (Delay + cost) / 2;
	else
		Delay = cost;

	debug_printf("心跳延迟 %dms / %dms \r\n", cost, Delay);

	return true;
}
