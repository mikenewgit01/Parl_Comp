#include <chrono>
#include <cstring>
#include <ctime>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "libs/exceptionpp/exception.h"

#include "src/msg_node.h"

msgpp::Message::Message(size_t id, std::string ip, std::string hostname, std::string message) : id(id), ip(ip), hostname(hostname), message(message) {}

size_t msgpp::Message::get_identifier() { return(this->id); }
std::string msgpp::Message::get_ip() { return(this->ip); }
std::string msgpp::Message::get_hostname() { return(this->hostname); }
std::string msgpp::Message::get_message() { return(this->message); }

std::vector<std::shared_ptr<msgpp::MessageNode>> msgpp::MessageNode::instances;
std::recursive_mutex msgpp::MessageNode::l;
std::chrono::milliseconds msgpp::MessageNode::increment = std::chrono::milliseconds(50);
sighandler_t msgpp::MessageNode::handler;

msgpp::MessageNode::MessageNode(size_t port, uint8_t protocol, size_t timeout, size_t max_conn) : protocol(protocol), port(port), timeout(timeout), max_conn(max_conn) {
	this->flag = std::shared_ptr<std::atomic<bool>> (new std::atomic<bool> (0));
	this->count = 0;
}

uint8_t msgpp::MessageNode::get_protocol() { return(this->protocol); }
size_t msgpp::MessageNode::get_port() { return(this->port); }
size_t msgpp::MessageNode::get_timeout() { return(this->timeout); }
size_t msgpp::MessageNode::get_max_conn() { return(this->max_conn); }
bool msgpp::MessageNode::get_status() { return(*(this->flag)); }
void msgpp::MessageNode::set_timeout(size_t timeout) { this->timeout = timeout; }

void msgpp::MessageNode::up() {
	{
		std::lock_guard<std::recursive_mutex> lock(msgpp::MessageNode::l);
		if(*flag == 1) {
			return;
		}
		if(msgpp::MessageNode::instances.size() == 0) {
			msgpp::MessageNode::handler = signal(SIGINT, msgpp::MessageNode::term);
		}
		*flag = 1;
		msgpp::MessageNode::instances.push_back(this->shared_from_this());
	}

	std::stringstream port;
	port << this->port;

	int server_sock, status;

		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	struct addrinfo info;
	struct addrinfo *list;
	memset(&info, 0, sizeof(info));

	if(this->protocol & msgpp::MessageNode::ipv6) {
		info.ai_family = AF_INET6;
	} else if(this->protocol & msgpp::MessageNode::ipv4) {
		info.ai_family = AF_INET;
	} else {
		info.ai_family = AF_UNSPEC;
	}
	info.ai_socktype = SOCK_STREAM;
	info.ai_flags = AI_PASSIVE;

	status = getaddrinfo(NULL, port.str().c_str(), &info, &list);
	if(list == NULL) {
		throw(exceptionpp::RuntimeError("msgpp::MessageNode::up", "cannot find address"));
	}

	server_sock = socket(list->ai_family, list->ai_socktype, list->ai_protocol);
	if(server_sock == -1) {
		freeaddrinfo(list);
		throw(exceptionpp::RuntimeError("msgpp::MessageNode::up", "cannot open socket"));
	}

	int yes = 1;
	status = setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
	status = bind(server_sock, list->ai_addr, list->ai_addrlen);
	if(status == -1) {
		freeaddrinfo(list);
		shutdown(server_sock, SHUT_RDWR);
		close(server_sock);
		throw(exceptionpp::RuntimeError("msgpp::MessageNode::up", "cannot bind server-side socket"));
	}

	status = listen(server_sock, this->get_max_conn());
	if(status == -1) {
		freeaddrinfo(list);
		shutdown(server_sock, SHUT_RDWR);
		close(server_sock);
		throw(exceptionpp::RuntimeError("msgpp::MessageNode::up", "cannot listen on server-side socket"));
	}

	// set as non-blocking
	//	cf. http://bit.ly/1tse7i3
	fcntl(server_sock, F_SETFL, O_NONBLOCK);

	this->threads.clear();

	// use sockaddr_storage for protocol-agnostic IP storage
	//	cf. http://bit.ly/1ukHOQ8
	struct sockaddr_storage client_addr;
	memset(&client_addr, sizeof(struct sockaddr_storage), sizeof(char));
	socklen_t client_size = 0;

	while(*(this->flag)) {
		// dispatch any incoming clients
		int client_sock = accept(server_sock, (sockaddr *) &client_addr, &client_size);
		if(client_sock != -1) {
			std::shared_ptr<std::thread> t (new std::thread(&msgpp::MessageNode::dispatch, this, client_sock, client_addr, client_size));
			this->threads.push_back(t);
		}
		std::this_thread::sleep_for(msgpp::MessageNode::increment);
	}

	// clean up client connections
	for(size_t i = 0; i < this->threads.size(); ++i) {
		this->threads.at(i)->join();
	}

	freeaddrinfo(list);
	shutdown(server_sock, SHUT_RDWR);
	close(server_sock);

	return;
}

void msgpp::MessageNode::dispatch(int client_sock, struct sockaddr_storage client_addr, socklen_t client_size) {
	char host[NI_MAXHOST] = "";
	char ip[NI_MAXHOST] = "";

	getnameinfo((struct sockaddr *) &client_addr, client_size, host, NI_MAXHOST, NULL, 0, 0);
	getnameinfo((struct sockaddr *) &client_addr, client_size, ip, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);

	fcntl(client_sock, F_SETFL, O_NONBLOCK);
	std::stringstream len_buf;
	std::stringstream msg_buf;
	bool is_done = 0;
	size_t size = 0;
	while(!is_done) {
		char tmp_buf[msgpp::MessageNode::size];
		memset(&tmp_buf, 0, msgpp::MessageNode::size);
		int n_bytes = 0;
		time_t start = time(NULL);
		while(((size_t) time(NULL) - start) < this->timeout) {
			n_bytes = recv(client_sock, &tmp_buf, msgpp::MessageNode::size, 0);
			if(n_bytes == -1) {
				if(errno != EAGAIN) {
					break;
				}
				std::this_thread::sleep_for(msgpp::MessageNode::increment);
			} else {
				break;
			}
		}
		if(n_bytes == 0 || n_bytes == -1) {
			// client closed unexpectedly
			// as the message queue is atomically set (i.e., no half-assed data), we will roll back changes and not touch the queue
			break;
		}
		std::string tmp = std::string(tmp_buf, n_bytes);
		if(size == 0) {
			size_t pos = tmp.find(':');
			if(pos == std::string::npos) {
				pos = tmp.size();
			}
			len_buf << tmp.substr(0, pos);
			if(pos != tmp.size()) {
				size = std::stoll(len_buf.str());
				msg_buf << tmp.substr(pos + 1);
			}
		} else {
			msg_buf << tmp;
		}
		if(size != 0) {
			is_done = (msg_buf.str().length() >= size);
		}
	}

	if(is_done && (msg_buf.str().length() >= size)) {
		std::lock_guard<std::mutex> lock(this->messages_l);
		this->messages.push_back(msgpp::Message(++this->count, ip, host, msg_buf.str().substr(0, size)));
	}
	shutdown(client_sock, SHUT_RDWR);
	close(client_sock);
}

void msgpp::MessageNode::dn() {
	std::lock_guard<std::recursive_mutex> lock(msgpp::MessageNode::l);
	if(*(this->flag) == 0) {
		return;
	}
	*(this->flag) = 0;

	// restore signal handler if no more instances are running
	if(msgpp::MessageNode::instances.size() == 0) {
		signal(SIGINT, msgpp::MessageNode::handler);
	}
}

size_t msgpp::MessageNode::query() {
	std::lock_guard<std::mutex> lock(this->messages_l);
	return(this->messages.size());
}

size_t msgpp::MessageNode::push(std::string message, std::string hostname, size_t port, bool silent_fail) {
	std::stringstream port_buf, msg_buf;
	port_buf << port;
	msg_buf << message.length() << ":" << message;

	int client_sock, status;
	struct addrinfo info;
	struct addrinfo *list;
	memset(&info, 0, sizeof(info));

	info.ai_family = AF_UNSPEC;
	info.ai_socktype = SOCK_STREAM;

	getaddrinfo(hostname.c_str(), port_buf.str().c_str(), &info, &list);
	if(list == NULL) {
		if(silent_fail) { return(0); }
		throw(exceptionpp::RuntimeError("msgpp::MessageNode::push", "cannot find endpoint"));
	}
	client_sock = socket(list->ai_family, list->ai_socktype, list->ai_protocol);
	if(client_sock == -1) {
		freeaddrinfo(list);
		shutdown(client_sock, SHUT_RDWR);
		close(client_sock);
		if(silent_fail) { return(0); }
		throw(exceptionpp::RuntimeError("msgpp::MessageNode::push", "cannot open socket"));
	}

	int yes = 1;
	status = setsockopt(client_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
	fcntl(client_sock, F_SETFL, O_NONBLOCK);

	time_t start = time(NULL);
	while(((size_t) time(NULL) - start) < this->timeout) {
		status = connect(client_sock, list->ai_addr, list->ai_addrlen);
		if(status != -1) {
			break;
		}
		std::this_thread::sleep_for(msgpp::MessageNode::increment);
	}

	if(status == -1) {
		freeaddrinfo(list);
		shutdown(client_sock, SHUT_RDWR);
		close(client_sock);
		if(silent_fail) { return(0); }
		throw(exceptionpp::RuntimeError("msgpp::MessageNode::push", "cannot connect to destination"));
	}

	int result = -1;
	size_t n_bytes = 0;

	start = time(NULL);
	while(((size_t) time(NULL) - start) < this->timeout) {
		result = send(client_sock, msg_buf.str().c_str(), msg_buf.str().length(), 0);
		if(result != -1) {
			n_bytes += result;
			if(n_bytes == msg_buf.str().length()) {
				break;
			}
		} else {
			if(errno == EWOULDBLOCK) {
				std::this_thread::sleep_for(msgpp::MessageNode::increment);
			} else {
				break;
			}
		}
	}

	if(result == -1 || n_bytes != msg_buf.str().length()) {
		freeaddrinfo(list);
		shutdown(client_sock, SHUT_RDWR);
		close(client_sock);
		if(silent_fail) { return(0); }
		throw(exceptionpp::RuntimeError("msgpp::MessageNode::send", "could not send data"));
	}

	freeaddrinfo(list);
	shutdown(client_sock, SHUT_RDWR);
	close(client_sock);

	return(result - (msg_buf.str().length() - message.length()));
}

std::string msgpp::MessageNode::pull(std::string hostname, bool silent_fail) {
	size_t target = 0;
	bool is_found = 0;

	time_t start = time(NULL);
	while(((size_t) time(NULL) - start) < this->timeout) {
		// lock during iteration, but unlock during sleep to give the queue an opportunity to fill up again
		{
			std::lock_guard<std::mutex> lock(this->messages_l);
			if(!this->messages.empty()) {
				for(std::vector<msgpp::Message>::iterator it = this->messages.begin(); it != this->messages.end(); ++it) {
					msgpp::Message instance = *it;
					bool match_h = (instance.get_hostname().compare("") == 0) || (instance.get_ip().compare("") == 0) || (hostname.compare("") == 0) || (hostname.compare(instance.get_hostname()) == 0) || (hostname.compare(instance.get_ip()) == 0);
					if(match_h) {
						target = it->get_identifier();
						is_found = 1;
						break;
					}
				}
			}
		}
		if(is_found) {
			break;
		}
		std::this_thread::sleep_for(msgpp::MessageNode::increment);
	}

	if(!is_found) {
		if(silent_fail) { return(""); }
		throw(exceptionpp::RuntimeError("msgpp::MessageNode::pull", "message does not exist"));
	}

	std::string message = "";
	{
		std::lock_guard<std::mutex> lock(this->messages_l);
		auto index = this->messages.end();
		for(std::vector<msgpp::Message>::iterator it = this->messages.begin(); it != this->messages.end(); ++it) {
			msgpp::Message instance = *it;
			if(instance.get_identifier() == target) {
				index = it;
				message = instance.get_message();
			}
		}
		this->messages.erase(index);
	}
	return(message);
}

/**
 * SIGINT handler -- terminate all running MessageNode instances
 */
void msgpp::MessageNode::term(int p) {
	std::lock_guard<std::recursive_mutex> lock(msgpp::MessageNode::l);
	for(std::vector<std::shared_ptr<msgpp::MessageNode>>::iterator it = msgpp::MessageNode::instances.begin(); it != msgpp::MessageNode::instances.end(); ++it) {
		(*it)->dn();
	}
	msgpp::MessageNode::instances.clear();
	// install old sighandler
	//	cf. http://bit.ly/1unilFS
	signal(SIGINT, msgpp::MessageNode::handler);
}
