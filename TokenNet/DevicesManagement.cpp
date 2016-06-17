﻿#include "DevicesManagement.h"
#include "Message\BinaryPair.h"

DevicesManagement* DevicesManagement::Current = nullptr;

/******************************** 功能代码 ********************************/

DevicesManagement::DevicesManagement()
{
	//DevArr.SetLength(0);
	//OnlineAlways.SetLength(0);
	Current = this;
}

DevicesManagement::~DevicesManagement()
{
	// for (int i = 0; i < Length(); i++)
	// {
	// 	auto dv = DevArr[i];
	// 	if (!dv)continue;
	// 	delete dv;
	// }
	//DevArr.SetLength(0);
	//OnlineAlways.SetLength(0);
	Current = nullptr;
}

int	DevicesManagement::PushDev(Device* dv)
{
	//DeviceRequest(DeviceAtions::Register, dv);
	DevArr.Add(dv);
	SaveDev();
	return DevArr.Count() - 1;
}

bool DevicesManagement::SetFlashCfg(uint addr, uint size)
{
	const uint minAddr = 0x8000000 + (Sys.FlashSize << 10) - (64 << 10);
	const uint maxAddr = 0x8000000 + (Sys.FlashSize << 10);
	if (addr<minAddr || addr>maxAddr)
	{
		debug_printf("设置的地址不在正确的位置上\r\n");
		return false;
	}
	const uint minSize = 4 << 10;
	const uint maxSize = 16 << 10;
	if (size<minSize || size>maxSize)
	{
		debug_printf("设置的大小不合适\r\n");
		return false;
	}
	_Addr = addr;
	_FlashSize = size;
	return true;
}

Device * DevicesManagement::FindDev(byte id) const
{
	if (id == 0)return nullptr;
	for (int i = 0; i < DevArr.Count(); i++)
	{
		auto dv	= (Device*)DevArr[i];
		if (id == dv->Address) return dv;
	}
	return nullptr;
}

Device * DevicesManagement::FindDev(const Buffer & hardid) const
{
	if (hardid.Length() == 0)return nullptr;
	for (int i = 0; i < DevArr.Count(); i++)
	{
		auto dv	= (Device*)DevArr[i];
		bool isEqual = true;
		if (dv != nullptr)
		{
			for (int j = 0; j < hardid.Length(); j++)
			{
				if (hardid[j] != dv->HardID[j])
				{
					isEqual = false;
					break;
				}
			}
		}
		if (isEqual) return dv;
	}
	return nullptr;
}

bool DevicesManagement::DeleteDev(byte id)
{
	TS("DevicesManagement::DeleteDevice");

	auto	dv = FindDev(id);
	if (dv&&dv->Address == id)
	{
		debug_printf("DevicesManagement::DeleteDev Del id: 0x%02X\r\n", id);

		int idx = DevArr.FindIndex(dv);
		debug_printf("idx~~~~~~~~~~~:%d\r\n", idx);
		if (idx >= 0)DevArr[idx] = nullptr;

		// 处理持久在线表
		int idx2 = OnlineAlways.FindIndex(dv);
		if (idx2 >= 0)OnlineAlways[idx2] = nullptr;

		delete dv;
		SaveDev();

		return true;
	}

	return false;
}

const Config DevicesManagement::GetStore(Flash & flash)
{
	if (_Addr == 0)
	{
		//debug_printf("还未设置设备储存区地址\r\n");
		//debug_printf("使用默认最后4K空间设置\r\n");
		const uint addr = 0x8000000 + (Sys.FlashSize << 10) - (4 << 10);
		SetFlashCfg(addr, 4 << 10);
	}
	Config cfg(flash, _Addr, _FlashSize);
	return cfg;
}

int DevicesManagement::LoadDev()
{
	TS("DevicesManagement::Load");

	debug_printf("DevicesManagement::Load Dev\r\n");
	Flash flash;
	auto cfg = GetStore(flash);

	byte* data = (byte*)cfg.Get("Devs");
	if (!data) return -1;

	Stream ms(data, _FlashSize);
	// 设备个数
	auto count = ms.ReadByte();
	debug_printf("\tCount %d\r\n", count);
	int i = 0;
	for (; i < count; i++)
	{
		//debug_printf("\t加载设备:");

		bool fs = false;
		/*ms.Seek(1);
		byte id = ms.Peek();
		ms.Seek(-1);*/
		// 前面一个字节是长度，第二个字节才是ID
		byte id = ms.GetBuffer()[1];
		auto dv = FindDev(id);
		if (!dv)
		{
			dv = new Device();
			fs = true;
		}
		dv->Read(ms);
		dv->Show();

		if (fs)
		{
			int idx = DevArr.FindIndex(nullptr);
			if (idx == -1)
			{
				if (dv->Valid())
					DevArr.Add(dv);
				else
					delete dv;
				//debug_printf("\t Push");
			}
		}
		debug_printf("\r\n");
	}

	debug_printf("Load %d Dev from 0x%08X\r\n", i, cfg.Address);

	byte len = DevArr.Count();
	debug_printf("Devices has %d Dev\r\n", len);
	// 如果加载得到的设备数跟存入的设备数不想等，则需要覆盖一次
	if (len != i) SaveDev();

	return i;
}

void DevicesManagement::SaveDev()
{
	TS("DevicesManagement::Save");

	Flash flash;
	auto cfg = GetStore(flash);

	byte buf[0x800];

	MemoryStream ms(buf, ArrayLength(buf));
	byte num = 0;
	for (int i = 0; i < DevArr.Count(); i++)
	{
		if (DevArr[i]) num++;
	}

	// 设备个数
	//int count = num;
	debug_printf("\tCount %d\r\n", num);
	ms.Write((byte)num);

	for (int i = 0; i < DevArr.Count(); i++)
	{
		auto dv = (Device*)DevArr[i];
		if (dv) dv->Write(ms);
	}
	debug_printf("DevicesManagement::SaveDevices Save %d Dev To 0x%08X！\r\n", num, cfg.Address);
	cfg.Set("Devs", Buffer(ms.GetBuffer(), ms.Position()));
}

void DevicesManagement::ClearDev()
{
	TS("DevicesManagement::ClearDev");

	Flash flash;
	auto cfg = GetStore(flash);

	debug_printf("DevicesManagement::ClearDevices Clear List 0x%08X \r\n", cfg.Address);

	DevArr.DeleteAll().Clear();
	/*for (int i = 0; i < DevArr.Count(); i++)
	{
		if (DevArr[i]) delete DevArr[i];
		DevArr[i] = nullptr;
	}*/
	//DevArr.SetLength(0);	// 清零后需要保存一下，否则重启后 Length 可能不是 0。做到以防万一
	SaveDev();
}

// 输出所有设备
void DevicesManagement::ShowDev()
{
	TS("DevicesManagement::DeviceShow");

	byte len = Length();
	byte count = 0;

	for (int i = 0; i < len; i++)
	{
		auto dv = (Device*)DevArr[i];
		if (dv == nullptr) continue;

		count++;
		dv->Show();
		debug_printf("\r\n");

		//Sys.Sleep(0);
	}
	debug_printf("\r\nHas %d Dev\r\n", count);
	debug_printf("\r\n\r\n");
}


/******************************** TokenMsg数据处理 ********************************/
/******************************** 收到Invoke ********************************/

void DevicesManagement::SetTokenClient(TokenClient *port)
{
	if (port == nullptr)return;
	Port = port;

	Port->Register("Device/List",	InvokeList		,this);
	Port->Register("Device/Update", InvokeUpdate	,this);
	Port->Register("Device/Delete", InvokeDelete	,this);
	Port->Register("Device/ListIDs", InvokeListIDs	,this);
}

// Invoke 注册项
bool DevicesManagement::InvokeList(void * param, const BinaryPair& args, Stream& result)
{
	if (param == nullptr)return false;
	auto dMgmt = (DevicesManagement*)param;
	return dMgmt->DeviceProcess(DeviceAtions::List, args, result);
}

bool DevicesManagement::InvokeUpdate(void * param, const BinaryPair& args, Stream& result)
{
	if (param == nullptr)return false;
	auto dMgmt = (DevicesManagement*)param;
	return dMgmt->DeviceProcess(DeviceAtions::Update, args, result);
}

bool DevicesManagement::InvokeDelete(void * param, const BinaryPair& args, Stream& result)
{
	if (param == nullptr)return false;
	auto dMgmt = (DevicesManagement*)param;
	return dMgmt->DeviceProcess(DeviceAtions::Delete, args, result);
}

bool DevicesManagement::InvokeListIDs(void * param, const BinaryPair& args, Stream& result)
{
	if (param == nullptr)return false;
	auto dMgmt = (DevicesManagement*)param;
	return dMgmt->DeviceProcess(DeviceAtions::ListIDs, args, result);
}
// 缺少更新的处理  未完待续
bool DevicesManagement::DeviceProcess(DeviceAtions act,const BinaryPair& args, Stream& result)
{
	TS("DevicesManagement::DeviceProcess");
	// 仅处理来自云端的请求
	switch (act)
	{
	case DeviceAtions::List:
	{
		// 获取需要发送的IDs
		ByteArray idss;
		args.Get("IDs", idss);
		for (int i = 0; i < idss.Length(); i++)	// 判定依据需要修改
		{
			// 获取数据ms
			MemoryStream dvms;
			// 序列化一个DevInfo到ms
			if (!GetDevInfo(idss[i], dvms))continue;
			// 转换为ByteArray
			ByteArray dvbs(dvms.GetBuffer(), dvms.Position());
			String countstr;
			// 这里需要注意  ！！！！！  i 是 int 类型
			countstr += i;
			// 写入DevInfo
			BinaryPair res(result);
			res.Set(countstr, dvbs);
		}
		return true;
	}

	case DeviceAtions::Delete:
	{
		// 拿到操作对象
		byte addr = 0;
		args.Get("ID", addr);
		if (addr != 0x00)
		{
			if (_DevProcess)_DevProcess(act, FindDev(addr), _ClbkParam);
			DeleteDev(addr);
		}

		// 拿到操作对象s
		ByteArray idss;
		args.Get("IDs", idss);
		if (idss.Length() > 0)
		{
			for (int i = 0; i < idss.Length(); i++)
			{
				// 外部处理一下
				if (_DevProcess)_DevProcess(act, FindDev(idss[i]), _ClbkParam);
				DeleteDev(idss[i]);
			}
		}

		// 准备回复数据
		if (addr == 0x00 && idss.Length() == 0)
		{
			debug_printf("DeleteDev Error\r\n");
			return false;
		}
	}
	break;

	case DeviceAtions::ListIDs:
	{
		//　获取IDs ByteArray
		MemoryStream idsms;
		WriteIDs(idsms);
		ByteArray idbs(idsms.GetBuffer(), idsms.Position());
		BinaryPair res(result);
		res.Set("IDs", idbs);
	}
	break;

	// 未写
	case DeviceAtions::Update:
	{
	}
	break;
	/*case DeviceAtions::Register:
		break;
	case DeviceAtions::Online:
		break;
	case DeviceAtions::Offline:
		break;*/
	default:
	{
		debug_printf("不支持的设备操作指令！！\r\n");
		return false;
	}
	}
	return true;
}

// 获取设备ID集合
int DevicesManagement::WriteIDs(Stream &ms)
{
	int len = 0;
	for (int i = 0; i < DevArr.Count(); i++)
	{
		auto dv = (Device*)DevArr[i];
		if (dv)
		{
			ms.Write(dv->Address);
			len++;
		}
	}
	return len;
}

// 获取设备信息到流
bool DevicesManagement::GetDevInfo(byte id, MemoryStream &ms)
{
	if (id == 0x00)return false;
	Device * dv = FindDev(id);
	return GetDevInfo(dv, ms);
}

// 获取设备信息到流
bool DevicesManagement::GetDevInfo(Device *dv, MemoryStream &ms)
{
	if (dv != nullptr)return false;

	BinaryPair bp(ms);

	MemoryStream dvms;
	BinaryPair dvbp(dvms);
	bp.Set("ID", dv->Address);

	byte login = dv->Logined ? 1 : 0;
	bp.Set("Online", login);
	bp.Set("Kind", dv->Kind);
	bp.Set("LastActive", dv->LastTime);
	bp.Set("RegisterTime", dv->RegTime);
	bp.Set("LoginTime", dv->LoginTime);
	// bp.Set("HardID", dv->Logins);

	bp.Set("Version", dv->Version);
	bp.Set("DataSize", dv->DataSize);
	bp.Set("ConfigSize", dv->ConfigSize);

	bp.Set("SleepTime", dv->SleepTime);
	bp.Set("Offline", dv->OfflineTime);
	bp.Set("PingTime", dv->PingTime);

	bp.Set("HardID", dv->HardID);
	bp.Set("Name", dv->Name);

	bp.Set("Password", dv->Pass);
	return true;
}


/******************************** 发送Invoke ********************************/


void DevicesManagement::SendDevicesIDs()
{
	// 获取IDList
	MemoryStream idms;
	WriteIDs(idms);
	ByteArray idbs(idms.GetBuffer(), idms.Position());
	// 封装成所需 Data
	MemoryStream ms;
	BinaryPair bp(ms);
	bp.Set("IDs", idbs);
	Buffer bs(ms.GetBuffer(), ms.Position());
	// 发送
	if (Port)Port->Invoke("Device/ListIDs", bs);
}

// 设备状态上报  一次一个设备
bool DevicesManagement::SendDevices(DeviceAtions act, const Device* dv)
{
	TS("DevicesManagement::SendDevices");
	// 不许为空
	if (dv == nullptr)return false;
	// 不存在主动发送这两条
	if (act == DeviceAtions::List || act == DeviceAtions::ListIDs)return false;
	// 保证可以顺利执行
	if (Port == nullptr)return false;
	if (Port->Status < 2) return false;
	// 只有 Register 需要发送完整设备信息
	String actstr;
	switch (act)
	{
	case DeviceAtions::Register:
		actstr = "Device/Register";
		break;
	case DeviceAtions::Update:
		actstr = "Device/Update";
		break;
	case DeviceAtions::Online:
		actstr = "Device/Online";
		break;
	case DeviceAtions::Offline:
		actstr = "Device/Offline";
		break;
	case DeviceAtions::Delete:
		actstr = "Device/Delete";
		break;
	default:
		debug_printf("无法处理的指令\r\n");
		return false;
		
	}
	if (actstr.Length() == 0)return false;
	// 拿取数据
	MemoryStream datams;
	BinaryPair bp(datams);
	GetDevInfo((Device*)dv, datams);

	if (Port)Port->Invoke(actstr, Buffer(datams.GetBuffer(),datams.Position()));
	return true;
}

/******************************** Tiny侧数据处理 ********************************/

void DevicesManagement::DeviceRequest(DeviceAtions act, byte id)
{
	if (id == 0x00)return;
	auto dv = FindDev(id);
	DeviceRequest(act, dv);
}
// 节点消息处理
void DevicesManagement::DeviceRequest(DeviceAtions act, const Device* dv)
{
	TS("DevicesManagement::DeviceRequest");

	bool PortOk = true;
	if (Port == nullptr)PortOk = false;
	if (Port->Status < 2) PortOk = false;
	if(Port == false)debug_printf("Port Not Realy\r\n");

	byte id = dv->Address;
	switch (act)
	{
	case DeviceAtions::List:
		return;
	case DeviceAtions::Update:
		return;
	case DeviceAtions::Online:
		debug_printf("节点上线 ID=0x%02X\r\n", id);
		break;
	case DeviceAtions::Offline:
		debug_printf("节点离线 ID=0x%02X\r\n", id);
		break;
	case DeviceAtions::Register:
		PushDev((Device*)dv);
		debug_printf("节点注册入网 ID=0x%02X\r\n", id);
		if(PortOk)SendDevices(act, dv);
		return;
	case DeviceAtions::Delete:
		debug_printf("节点删除~~ ID=0x%02X\r\n", id);
		DeleteDev(id);
		//if(PortOk)SendDevices(act, dv);
		break;
	default:
		debug_printf("无法识别的节点操作 Act=%d ID=0x%02X\r\n", (byte)act, id);
		break;
	}
}

/******************************** 其他 ********************************/

void DevicesManagement::MaintainState()
{
	if (Port)return;
	if (Port->Status < 2) return;
	SendDevicesIDs();

	auto now = Sys.Seconds();

	// 处理持久在线设备
	for (int i = 0; i < OnlineAlways.Count(); i++)
	{
		auto dv = (Device*)OnlineAlways[i];
		if (dv) dv->LastTime = now;
	}

	byte len = Length();
	for (int i = 0; i < len; i++)
	{
		auto dv = (Device*)DevArr[i];
		if (!dv) continue;

		ushort time = dv->OfflineTime ? dv->OfflineTime : 60;

		// 特殊处理网关自身
		// if (dv->Address == gw->Server->Cfg->Address) dv->LastTime = now;

		if (dv->LastTime + time < now)
		{	// 下线
			if (dv->Logined)
			{
				//debug_printf("设备最后活跃时间：%d,系统当前时间:%d,离线阈值:%d\r\n",dv->LastTime,now,time);
				DeviceRequest(DeviceAtions::Offline, dv);
				dv->Logined = false;
			}
		}
		else
		{	// 上线
			if (!dv->Logined)
			{
				DeviceRequest(DeviceAtions::Online, dv);
				dv->Logined = true;
			}
		}
	}
}

