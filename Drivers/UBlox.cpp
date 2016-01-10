﻿#include "UBlox.h"
#include "SerialPort.h"

UBlox::UBlox()
{
	Name	= "UBlox";
}

bool UBlox::OnOpen(bool isNew)
{
	if(isNew)
	{
		auto sp	= (SerialPort*)Port;
		sp->Rx.SetCapacity(1024);
		sp->MaxSize		= 1024;
		sp->ByteTime	= 20;	// 拆包间隔
	}

	return true;
}

bool UBlox::SetBaudRate(int baudRate)
{
	if(!Open()) return false;

	// 构造波特率指令。默认115200
	byte cmd[] = {
		0xB5, 0x62, 0x06, 0x00, 0x14, 0x00,
		0x01, // PortID
		0x00, // reserved0
		0x00, 0x00, // txReady
		0xD0, 0x08, 0x00, 0x00, // mode。	0000 | 2reserved1 | 2charLen + 0 | parity | nStopBits
		0x00, 0xC2, 0x01, 0x00, // baudRate
		0x07, 0x00, // inProtoMask。	inUbx | inNmea | inRtcm
		0x03, 0x00, // outProtoMask。	outUbx | outNmea
		0x00, 0x00, // flags。0 | extendedTxTimeout
		0x00, 0x00, // reserved5
		0xC0, 0x7E	// CK_A CK_B
		};
	int len	= ArrayLength(cmd);
	int p	= 6 + 8;	// 头6字节，偏移8
	memcpy(cmd + p, &baudRate, 4);	// 小字节序
	// 修改校验。不含头尾各2字节
	byte CK_A = 0, CK_B = 0;
	for(int i=2; i<len-2-2; i++)
	{
		CK_A = CK_A + cmd[i];
		CK_B = CK_B + CK_A;
	}
	cmd[len - 2]	= CK_A;
	cmd[len - 1]	= CK_B;
	// 发送命令
	Port->Write(Array(cmd, len));

	// 修改波特率，重新打开
	Close();

	auto sp	= (SerialPort*)Port;
	sp->SetBaudRate(baudRate);

	return Open();
}

void UBlox::OnReceive(const Array& bs, void* param)
{
	TS("UBlox::OnReceive");

	debug_printf("GPS[%d]=", bs.Length());
	String str(bs);
	str.Show(true);

	if(Buffer.Capacity() == 0) return;

	// 避免被截成两段
	if(bs[0] != '$')
	{
		if(Buffer.Length() == 0)
		{
			/*debug_printf("GPS数据断片[%d]=", bs.Length());
			String str(bs);
			str.Show(true);*/
		}

		//GPSDATA	+= bs;
	}
	else
	{
		Buffer.SetLength(0);
		Buffer.Copy(bs);
	}
}