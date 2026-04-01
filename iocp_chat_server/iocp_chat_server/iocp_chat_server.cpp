#include <iostream>
#include <WS2tcpip.h>
#include <unordered_map>
#include <MSWSock.h>
#pragma comment(lib, "WS2_32.lib")
#pragma comment(lib, "MSWSock.lib")
using namespace std;
constexpr int PORT_NUM = 3500;
constexpr int BUF_SIZE = 200;

#pragma pack(push, 1)
struct PACKET {
	unsigned char size;
	unsigned char sender_id;
	char message[BUF_SIZE];
};
#pragma pack(pop)

enum IOType { IO_SEND, IO_RECV, IO_ACCEPT };

class EXP_OVER {
public:
	WSAOVERLAPPED m_over;
	IOType m_iotype;			// 여기 들어오는 값을 보고 이게 Accept인지 Recv인지 Send인지 구분 가능
	WSABUF	m_wsa;
	char  m_buff[BUF_SIZE];
	EXP_OVER() {
		ZeroMemory(&m_over, sizeof(m_over));
		m_wsa.buf = m_buff;
		m_wsa.len = BUF_SIZE;
	}
	EXP_OVER(IOType iot) : m_iotype(iot)
	{
		ZeroMemory(&m_over, sizeof(m_over));
		m_wsa.buf = m_buff;
		m_wsa.len = BUF_SIZE;
	}
};

class SESSION;
unordered_map<long long, SESSION> clients;

class SESSION {
	SOCKET client;
	long long m_id;
public:
	EXP_OVER recv_over;
	int m_prev_recv = 0;		// unsigned char해도 되지만 나중에 BUF_SIZE를 확장하면 버퍼 오버플로우가 작동할 수 있음.

	CHAR recv_mess[BUF_SIZE];
	SESSION() { exit(-1); }
	SESSION(int id, SOCKET so) : m_id(id), client(so)
	{

	}
	~SESSION()
	{
		closesocket(client);
	}
	void do_recv()
	{
		DWORD recv_flag = 0;
		memset(&recv_over.m_over, 0, sizeof(recv_over.m_over));

		recv_over.m_iotype = IO_RECV;

		recv_over.m_wsa.buf = recv_over.m_buff + m_prev_recv;
		recv_over.m_wsa.len = BUF_SIZE - m_prev_recv;

		WSARecv(client, &recv_over.m_wsa, 1, 0, &recv_flag, &recv_over.m_over, nullptr);
	}
	void do_send(PACKET *p)
	{
		EXP_OVER* o = new EXP_OVER(IO_SEND);
		memcpy(o->m_buff, p, p->size);
		o->m_wsa.len = p->size;
		WSASend(client, &o->m_wsa, 1, 0, 0, &o->m_over, nullptr);
	}
};

void error_display(const wchar_t* msg, int err_no)
{
	WCHAR* lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_no,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	std::wcout << msg;
	std::wcout << L" === 에러 " << lpMsgBuf << std::endl;
	while (true);   // 디버깅 용
	LocalFree(lpMsgBuf);
}

//void CALLBACK recv_callback(DWORD err, DWORD num_bytes, LPWSAOVERLAPPED over, DWORD flags)
//{
//	int client_id = static_cast<int>(reinterpret_cast<long long>(over->hEvent));
//	cout << "Client[" << client_id << "] sent: " << clients[client_id].c_mess << endl;
//	for (auto& cl : clients)		// 모든 클라이언트에게 메시지 전송
//		cl.second.do_send(client_id, num_bytes, clients[client_id].c_mess);
//	clients[client_id].do_recv();
//}

//void CALLBACK send_callback(DWORD err, DWORD num_bytes, LPWSAOVERLAPPED over, DWORD flags)
//{
//	EXP_OVER* o = reinterpret_cast<EXP_OVER*>(over);
//	delete o;
//}

int main()
{
	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);
	SOCKET server = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	SOCKADDR_IN server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(PORT_NUM);
	server_addr.sin_addr.S_un.S_addr = INADDR_ANY;
	bind(server, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
	listen(server, SOMAXCONN);
	HANDLE h_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	CreateIoCompletionPort((HANDLE)server, h_iocp, 0, 0);
	// CreateIoCompletionPort는 인자에 따라 하는 일이 달라진다. 첫 번째 인자에 소켓 대신 INVALID_HANDLE_VALUE를 넣으면 IOCP를 새로 생성하는 것이고, 소켓을 넣으면 해당 소켓과 IOCP를 연결해준다.

	SOCKET client_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);			// 빈 소켓 생성
	EXP_OVER accept_over(IO_ACCEPT);
	AcceptEx(server, client_socket, &accept_over.m_buff, 0, sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, NULL, &accept_over.m_over);		// 운영체제에게 토스
	
	int client_num = 0;
	while(true) {
		DWORD num_bytes;
		ULONG_PTR key;
		LPOVERLAPPED over;
		GetQueuedCompletionStatus(h_iocp, &num_bytes, &key, &over, INFINITE);		// over에서 내용 보관
		if (over == nullptr) {
			error_display(L"GQCS Error: ", WSAGetLastError());
			continue;
		}
		EXP_OVER* exp_over = reinterpret_cast<EXP_OVER*>(over);
		switch (exp_over->m_iotype) {
		case IO_ACCEPT:
			client_num++;
			cout << "Client connected." << endl;
			CreateIoCompletionPort((HANDLE)client_socket, h_iocp, client_num, 0);
			clients.try_emplace(client_num, client_num, client_socket);
			clients[client_num].do_recv();
			client_socket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
			AcceptEx(server, client_socket, &accept_over.m_buff, 0, sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, NULL, &accept_over.m_over);
			break;

		case IO_RECV: {
			int client_id = static_cast<int>(key);		// 방금 서버로 채팅을 보낸 사람의 id
			
			int remain_data = clients[client_id].m_prev_recv + num_bytes;
			char* packet_start = clients[client_id].recv_over.m_buff;

			while (remain_data > 0) {
				int packet_size = static_cast<unsigned char>(packet_start[0]);		// 패킷의 첫 번째 바이트는 크기 정보를 갖고있음

				if (packet_size == 0 || packet_size > remain_data) break;		// 더 받을 데이터가 있다면 받고오기

				// 여기까지 왔다면 받을 데이터가 다 도착했다는 뜻
				PACKET* packet = reinterpret_cast<PACKET*>(packet_start);
				packet->sender_id = client_id;

				cout << "Client[" << client_id << "] sent: " << packet->message << endl;

				// 브로드캐스팅
				for (auto& cl : clients) {		// 모든 클라이언트에게 메시지 전송
					cl.second.do_send(packet);
				}

				packet_start += packet_size;
				remain_data -= packet_size;
			}

			if (remain_data > 0) {
				memmove(clients[client_id].recv_over.m_buff, packet_start, remain_data);
			}
			
			clients[client_id].m_prev_recv = remain_data;
			clients[client_id].do_recv();
			}
			break;

		case IO_SEND: {
			EXP_OVER* o = reinterpret_cast<EXP_OVER*>(over);
			delete o;
			}
			break;
		}
	}

	//SOCKADDR_IN cl_addr;
	//int addr_size = sizeof(cl_addr);
	//for (int i = 1; ; ++i) {
	//	SOCKET client = WSAAccept(server,
	//		reinterpret_cast<sockaddr*>(&cl_addr), &addr_size, NULL, NULL);
	//	clients.try_emplace(i, i, client);
	//	clients[i].do_recv();
	//}

	closesocket(server);
	WSACleanup();
}
