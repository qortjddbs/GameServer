#include <iostream>
#include <SDKDDKVER.h>
#include <boost/asio.hpp>

using namespace std;

int main()
{
    try {
        boost::asio::io_context io_context;                 // ASIO 객체
		boost::asio::ip::tcp::socket socket(io_context);    // 어떤 ASIO 객체에 소켓을 연결할 것인지 지정    
        boost::asio::ip::tcp::endpoint server_addr(boost::asio::ip::address::from_string("127.0.0.1"), 3500);   // 인터넷 주소는 endpoint객체로 관리
        boost::asio::connect(socket, &server_addr);         // (이 소켓을, 이 주소에 연결시켜라)
        
        for (;;) {
            std::string buf;
            boost::system::error_code error;

            std::cout << "Enter Message: ";
            std::getline(std::cin, buf);
            if (0 == buf.size()) break;

            // write -> 블로킹
			// write_some -> 보낼 수 있는 만큼만 보내고 리턴
            socket.write_some(boost::asio::buffer(buf), error);
            if (error == boost::asio::error::eof) break;
            else if (error) throw boost::system::system_error(error);

            char reply[1024];
			// read 하면 1024바이트를 받을 때까지 기다림
			// read_some 하면 받을 수 있는 만큼만 받고 리턴
            size_t len = socket.read_some(boost::asio::buffer(reply), error);
            if (error == boost::asio::error::eof) break;
            else if (error) throw boost::system::system_error(error);

            reply[len] = 0;
            std::cout << len << " bytes received: " << reply << endl;
        }
    }
    catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
}




