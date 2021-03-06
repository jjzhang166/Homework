#include "Dispatch.h"

#include <network/network.h>

#include "__send_buf.h"
#include "msg.h"
#include "Counter.h"

#include <x_vs/xvs.h>
#include <utils/time.h>

Dispatch* Dispatch::_instance = NULL;

Dispatch::Dispatch(void)
{
	//pthread_rwlock_init(&_opLock, NULL);

	for(int i = 0; i < CAPACITY; i++)
	{
		ITEMS items;
		_outportCan.insert(OUTPORTS::value_type(new Outport(i), items));
	}
}


Dispatch::~Dispatch(void)
{
	for(OUTPORTS::iterator it = _outportCan.begin(); it != _outportCan.end(); it++)
	{
		delete it->first;
		it->second.clear();
	}
	_outportCan.clear();

	//pthread_rwlock_destroy(&_opLock);
}

int Dispatch::Init()
{
	for(OUTPORTS::iterator it = _outportCan.begin(); it != _outportCan.end(); it++)
	{
		it->first->Init();
	}

	return 0;
}

int Dispatch::Deinit()
{
	for(OUTPORTS::iterator it = _outportCan.begin(); it != _outportCan.end(); it++)
	{
		it->first->Deinit();
	}

	return 0;
}

int Dispatch::add(u_int32 ipaddr, u_int16 port)
{
	//pthread_rwlock_wrlock(&_opLock);

	Outport* op = _outportCan.begin()->first;
	for(OUTPORTS::iterator it = _outportCan.begin(); it != _outportCan.end(); it++)
	{
		if(it->second.size() < _outportCan[op].size())
		{
			op = it->first;
		}
		else
		{
			continue;
		}
	}
	_outportCan[op].insert(ITEMID(ipaddr, port));

	//telnet监视用
	g_OutputList.insert(ITEMID(ipaddr, port));
	//pthread_rwlock_unlock(&_opLock);

	LOG(LEVEL_INFO, "Outport(%d) size(%d) add!\n", (int)op, _outportCan[op].size());
	return op->add(ipaddr, port);
}

int Dispatch::del(u_int32 ipaddr, u_int16 port)
{
	//pthread_rwlock_wrlock(&_opLock);

	Outport* op = NULL;//_outportCan.begin()->first;
	for(OUTPORTS::iterator it = _outportCan.begin(); it != _outportCan.end(); it++)
	{
		if(it->second.find(ITEMID(ipaddr, port)) != it->second.end())
		{
			op = it->first;
			break;
		}
	}

	if(NULL == op)
	{
		LOG(LEVEL_INFO, "Can't find output(%x:%hu).\n", ipaddr, port);
		return -1;
	}

	_outportCan[op].erase(ITEMID(ipaddr, port));

	//telnet监视用
	g_OutputList.erase(ITEMID(ipaddr, port));
	//pthread_rwlock_unlock(&_opLock);
	
	LOG(LEVEL_INFO, "Outport(%d) size(%d) del!\n", (int)op, _outportCan[op].size());
	return op->del(ipaddr, port);
}

int Dispatch::push(u_int32 ipaddr, u_int16 port, AUTOMEM* mem)
{
	//pthread_rwlock_rdlock(&_opLock);

	Outport* op = _outportCan.begin()->first;
	for(OUTPORTS::iterator it = _outportCan.begin(); it != _outportCan.end(); it++)
	{
		if(it->second.find(ITEMID(ipaddr, port)) != it->second.end())
		{
			op = it->first;
			break;
		}
	}

	//pthread_rwlock_unlock(&_opLock);

	return op->push(ipaddr, port, mem);
}


/********************************--Outport class--*********************************/
Outport::Outport(int i) : _port_segment(i)
{
}

Outport::~Outport()
{
	for(std::list<data_common*>::iterator it = _command_list.begin(); it != _command_list.end(); it++)
	{
		delete (*it);
	}
	_command_list.clear();

	for(std::map<u_int64,item*>::iterator it = _item_list.begin(); it != _item_list.end(); it++)
	{
		// 
		for(std::list<packet*>::iterator it2 = (*it).second->packet_list.begin();
			it2 != (*it).second->packet_list.end();
			it2++)
		{
			gCntE(opt_AUTOMEM_Output);
			AUTOMEM* mem = (AUTOMEM*)(*it2)->data;
			mem->release();

			delete (*it2);
		}

		//
		delete (it->second);
	}
	_item_list.clear();
}

int Outport::Init()
{
	//
	pthread_mutex_init(&_mutex, NULL);

	//
	for(unsigned int i = 0, j = 0; i < SOCKET_POOL_SIZE; i++)
	{
		SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);

		for(; true; j++)
		{
			sockaddr_in sa;
			sa.sin_family = AF_INET;
			sa.sin_addr.s_addr = INADDR_ANY;//inet_addr("127.0.0.1");
			sa.sin_port = htons(FIRST_PORT + _port_segment*1000 + j);
			if(bind(s, (sockaddr*)&sa, sizeof(sa)) != SOCKET_ERROR)
			{
				break;
			}
		}

		Network::DefaultDatagram(s);

		Network::SetNonBlock(s);

		_socket_pool.push_back(s);
	}

	//
	if(evutil_socketpair(AF_INET, SOCK_STREAM, 0, _pipe) != 0)
	{
		return -1;
	}

	//
	if(evutil_socketpair(AF_INET, SOCK_STREAM, 0, _exit) != 0)
	{
		return -1;
	}

	Network::DefaultStream(_pipe[0]);
	Network::DefaultStream(_pipe[1]);
	Network::DefaultStream(_exit[0]);
	Network::DefaultStream(_exit[1]);

	//
	if(BeginThread() != 0)
	{
		return -1;
	}

	return 0l;
}

int Outport::Deinit()
{
	//
	if(EndThread() != 0)
	{
		return -1;
	}

	//
	evutil_closesocket(_pipe[0]);
	evutil_closesocket(_pipe[1]);

	//
	evutil_closesocket(_exit[0]);
	evutil_closesocket(_exit[1]);

	//
	for(std::vector<SOCKET>::iterator it = _socket_pool.begin();
		it != _socket_pool.end();
		it++)
	{
		Network::Close((*it));
	}
	_socket_pool.clear();

	//
	pthread_mutex_destroy(&_mutex);

	//
	return 0l;
}

int Outport::BeginThread()
{
	if(pthread_create(&_thread, NULL, Outport::ThreadFunc, this) != 0)
	{
		return -1;
	}

	return 0l;
}

int Outport::EndThread()
{
	//
	exitloop();

	//
	pthread_join(_thread, NULL); 

	//
	return 0l;
}

int Outport::add(u_int32 ipaddr, u_int16 port)
{
	data_add* d = new data_add;
	d->cmd = cmd_add;
	d->ipaddr = ipaddr;
	d->port = port;

	command((data_common*)d);

	return 0l;
}

int Outport::del(u_int32 ipaddr, u_int16 port)
{
	data_del* d = new data_del;
	d->cmd = cmd_del;
	d->ipaddr = ipaddr;
	d->port = port;

	command((data_common*)d);

	return 0l;
}

int Outport::push(u_int32 ipaddr, u_int16 port, AUTOMEM* mem)
{
	gCntB(opt_AUTOMEM_Output);
	mem->ref();

	data_push* d = new data_push;
	d->cmd = cmd_push;
	d->ipaddr = ipaddr;
	d->port = port;
	d->datalen = 0;
	d->data = (char*)mem;
	
	command((data_common*)d);

	return 0l;
}


int Outport::command(data_common* d)
{
	pthread_mutex_lock(&_mutex); 
	_command_list.push_back(d);
	pthread_mutex_unlock(&_mutex);

	//
	char flag = 0x0;
	if(::send(_pipe[0], &flag, sizeof(flag), 0) != sizeof(flag))
	{
		return -1;
	}

	//
	return 0;

}

Outport::data_common* Outport::command()
{
	//
	char flag = 0x0;
	if(::recv(_pipe[1], &flag, sizeof(flag), 0) != sizeof(flag))
	{
		return NULL;
	}

	//
	data_common* ret = NULL;

	//
	pthread_mutex_lock(&_mutex); 
	if(_command_list.size() > 0)
	{
		ret = _command_list.front();
		_command_list.pop_front();
	}
	pthread_mutex_unlock(&_mutex);

	//
	return ret;
}

int Outport::exitloop()
{
	char flag = 0x0;
	::send(_exit[0], &flag, sizeof(flag), 0);
	return 0l;
}

void* Outport::ThreadFunc(void* data)
{
	Outport* pThis = reinterpret_cast<Outport*>(data);
	pThis->Run();
	return NULL;
}

void Outport::Run()
{
	u_int64 basetime = Counter::instance()->tick();

	while(true)
	{
		FD_SET fdset;
		FD_ZERO(&fdset);

		FD_SET(_pipe[1], &fdset);
		FD_SET(_exit[1], &fdset);

		struct timeval tv = {0, TICK_TIME};
		int ret = Network::Select(FD_SETSIZE, &fdset, NULL, NULL, &tv);
		if(ret > 0)
		{
			if(FD_ISSET(_pipe[1], &fdset))
			{
				data_common* d = command();
				if(d)
				{
					process(d);
				}

			}

			if(FD_ISSET(_exit[1], &fdset))
			{
				break;
			}

		}

		u_int64 nowtime = Counter::instance()->tick();

		u_int64 diff = nowtime - basetime;
		if(diff >= TICK_TIME / 1000)
		{
			//
			basetime = nowtime;

			// 
			send(diff*1000/TICK_TIME);
		}

	}
}

void Outport::process(data_common* d)
{
	if(d->cmd == cmd_push)
	{
		data_push* p = (data_push*)d;

		std::map<u_int64,item*>::iterator it = _item_list.find(ITEMID(p->ipaddr, p->port));
		if(it != _item_list.end())
		{
#ifdef OPT_MEM_TO_PTR
			AUTOMEM* mem = (AUTOMEM *)p->data;
			if(mem->isVideo())
			{
				VS_VIDEO_PACKET* vsp = reinterpret_cast<VS_VIDEO_PACKET*>(mem->get());
				int sc = SplitFrame_Push(vsp, &((*it).second->packet_list));

				gCntE(opt_AUTOMEM_Output);
				mem->release();
				LOG(LEVEL_INFO, "vsp, frameno(%u), stamp(%llu)", vsp->framenum, vsp->timestamp);
			}
			else
			{
				packet* m = new packet;
				m->datalen = 0; //no use
				m->data = p->data;

				(*it).second->packet_list.push_back(m);
				VS_AUDIO_PACKET* asp = reinterpret_cast<VS_AUDIO_PACKET*>(mem->get());
				LOG(LEVEL_INFO, "asp, frameno(%u), stamp(%llu)", asp->framenum, asp->timestamp);
			}
#else
			packet* m = new packet;
			m->datalen = 0; //no use
			m->data = p->data;

			(*it).second->packet_list.push_back(m);
#endif//OPT_MEM_TO_PTR
		}
		else
		{
			gCntE(opt_AUTOMEM_Output);
			AUTOMEM* mem = (AUTOMEM *)p->data;
			mem->release();
		}

		delete p;
	}
	else if(d->cmd == cmd_add)
	{
		data_add* p = (data_add*)d;

		item* i = new item;
		i->ipaddr = p->ipaddr;
		i->port = p->port;

		_item_list[ITEMID(p->ipaddr, p->port)] = i;

		delete p;

	}
	else if(d->cmd == cmd_del)
	{
		data_del* p = (data_del*)d;

		std::map<u_int64,item*>::iterator it = _item_list.find(ITEMID(p->ipaddr, p->port));
		if(it != _item_list.end())
		{
			// 
			for(std::list<packet*>::iterator it2 = (*it).second->packet_list.begin();
				it2 != (*it).second->packet_list.end();
				it2++)
			{
				gCntE(opt_AUTOMEM_Output);
				AUTOMEM* mem = (AUTOMEM*)(*it2)->data;
				mem->release();

				delete (*it2);
			}

			//
			delete (it->second);

			//
			_item_list.erase(it);
		}

		delete p;
	}
	else
	{
		LOG(LEVEL_ERROR, "don't support command(%d)", d->cmd);
	}
}

void Outport::send(int loop)
{
	if(loop > 4)
	{
		LOG(LEVEL_INFO, "#################### LOOP=%d", loop);
	}

	while(loop-- > 0)
	{
		//
		std::list<unsigned int> cntlist;		

		for(std::map<u_int64, item*>::iterator it = _item_list.begin();
			it != _item_list.end();
			it++)
		{	
			if((*it).second->packet_list.size() > 0)
			{
				cntlist.push_back((*it).second->packet_list.size() / TICK_COUNT + 1);
			}
			else
			{
				cntlist.push_back(0);
			}
		}

		// 发送数据
		while(1)
		{
			//
			bool bsend = false;

			//
			std::list<unsigned int>::iterator it1 = cntlist.begin();
			std::map<u_int64, item*>::iterator it2 = _item_list.begin();
			for( ; it1 != cntlist.end() && it2 != _item_list.end(); it1++, it2++)
			{
				if((*it1) > 0)
				{
					// 置发送标识
					bsend = true;

					// 减小需要发送数量
					(*it1)--;

					// init
					item* p1 = (*it2).second;
					packet* p2 = (*it2).second->packet_list.front();

					AUTOMEM* mem = (AUTOMEM*)p2->data;
										
					// 
					__send_buf(_socket_pool[p1->port % SOCKET_POOL_SIZE], p1->ipaddr, p1->port, mem->size(), mem->get());

//#ifdef OPT_DEBUG_OUT
	VS_VIDEO_PACKET* packet = (VS_VIDEO_PACKET*)mem->get();

	struct timeval tv;
	gettimeofday(&tv, NULL);

	ULONGLONG nowtime = ((ULONGLONG)tv.tv_sec * 1000 + tv.tv_usec/1000);

	g_dif = packet->timestamp - nowtime;

if((g_dbgClass & opt_Dispatch) != 0)
{
	if(packet->slicecnt > 3 || g_dif < 0)
	{
		union
		{
			unsigned char v[4];
			unsigned int  value;
		}ip;

		ip.value = p1->ipaddr;
	
		pthread_t self_id = pthread_self();
		LOG(LEVEL_INFO, "thread=%d:%u, packet.size=%d, loop=%d, SEND >> addr=%d.%d.%d.%d, port=%d. stamp=%llu, framenum=%u, slicecnt=%u, slicenum=%u,nowtimestamp=%llu, dif=%ld.\n"
			, (int)self_id.p, self_id.x
			, p1->packet_list.size(), loop
			, ip.v[3], ip.v[2], ip.v[1], ip.v[0], p1->port
			, packet->timestamp, packet->framenum, packet->slicecnt, packet->slicenum, nowtime, g_dif);
	}
}
//#endif//OPT_DEBUG_OUT
					//
					gCntE(opt_AUTOMEM_Output);
					mem->release();
					delete p2;
					p1->packet_list.pop_front();

				}

			}

			// 如果没有数据发送 表示不需要
			if(!bsend)
			{
				return;
			}

		}

	}
}

int Outport::SplitFrame_Push(VS_VIDEO_PACKET* vsp, std::list<packet*>* plp)
{
	//取出内存结构体，获取码流数据
	OPT_MEM* omem = (OPT_MEM*)vsp->data;
//	OPT_SHR_MEM* om_data = (OPT_SHR_MEM*)omem->memData;

	unsigned int slicecnt = omem->memLen / VDATA_SLICE_LEN;
	if(omem->memLen % VDATA_SLICE_LEN != 0)
	{
		slicecnt += 1;
	}

	//
	unsigned int used_len = 0;
	unsigned int i = 0;
	for(i = 0; i < slicecnt; i++)
	{
		//
		unsigned int data_size = omem->memLen - used_len;
		if(data_size > VDATA_SLICE_LEN)
		{
			data_size = VDATA_SLICE_LEN;
		}

		//
		unsigned int sumlen = sizeof(VS_VIDEO_PACKET) + data_size;

		gCntB(opt_AUTOMEM_Output);
		AUTOMEM* mem = new AUTOMEM(sumlen);
		VS_VIDEO_PACKET* vspSlice = reinterpret_cast<VS_VIDEO_PACKET*>(mem->get());

		vspSlice->packet_type = 0x0;
		vspSlice->signal_id = vsp->signal_id;
		vspSlice->stream_id = vsp->stream_id;
		vspSlice->base_id = vsp->base_id;
		vspSlice->framenum = vsp->framenum;
		vspSlice->sys_basenum = vsp->sys_basenum;
		vspSlice->sys_framenum = vsp->sys_framenum;
		vspSlice->replay = vsp->replay;
		vspSlice->slicecnt = slicecnt;
		vspSlice->slicenum = i;
		vspSlice->timestamp = vsp->timestamp;
		vspSlice->area = vsp->area;
		vspSlice->resol = vsp->resol;
		vspSlice->valid = vsp->valid;
		vspSlice->codec = vsp->codec;
		vspSlice->frametype = vsp->frametype;
		vspSlice->datalen = data_size;
		memcpy(vspSlice->data, omem->memData + used_len, data_size);
		//memcpy(vspSlice->data, om_data->sp.get() + used_len, data_size);

		//
		used_len += data_size;
				
		//
		packet* m = new packet();
		m->data = (char*)mem;
		m->datalen = 0;//nouse
		plp->push_back(m);
	}

	//delete[] omem->memData;
	//共享指针的回收随VideoSlice
	//delete om_data;
	//om_data = NULL;
	return i;
}