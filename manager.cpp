#ifdef WIN32
#include <windows.h>
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif
 
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <ctype.h>
#include <string>
#include <iostream> 
#include <fstream>
using namespace std;
#include "libssh2.h"
#include "libssh2_sftp.h"
#include "ssh2.h"

Channel::Channel( LIBSSH2_CHANNEL *channel)
        :m_channel(channel)
    {
        
    }
    
    Channel::~Channel(void)
    {
        if (m_channel)
        {
            libssh2_channel_free(m_channel);
            m_channel = NULL;
        }
    }
    
    string Channel::Read( const string &strend, int timeout )
    {
        string data;
        if( NULL == m_channel )
        {
            return data;
        }
        
        LIBSSH2_POLLFD *fds = new LIBSSH2_POLLFD;
        fds->type = LIBSSH2_POLLFD_CHANNEL;
        fds->fd.channel = m_channel;
        fds->events = LIBSSH2_POLLFD_POLLIN | LIBSSH2_POLLFD_POLLOUT;
        
        if( timeout % 50 )
        {
            timeout += timeout %50;
        }
        while(timeout>0)
        {
            int rc = (libssh2_poll(fds, 1, 10));
            if (rc < 1)
            {
                timeout -= 50;
                usleep(50*1000);
                continue;
            }
            
            if ( fds->revents & LIBSSH2_POLLFD_POLLIN )
            {
                char buffer[64*1024] = "";
                size_t n = libssh2_channel_read( m_channel, buffer, sizeof(buffer) );
                if ( n == LIBSSH2_ERROR_EAGAIN )
                {
                    //fprintf(stderr, "will read again\n");
                }
                else if (n <= 0)
                {
                    return data;
                }
                else
                {
                    data += string(buffer);
                    if( "" == strend )
                    {
                        return data;
                    }
                    size_t pos = data.rfind(strend);
                    if( pos != string::npos && data.substr(pos, strend.size()) == strend  )
                    {
                        return data;
                    }
                }
            }
            timeout -= 50;
            usleep(50*1000);
        }
        
        cout<<"read data timeout"<<endl;
        return data;
    }
    
    bool Channel::Write(const string &data)
    {
        if( NULL == m_channel )
        {
            return false;
        }
        
        string send_data = data + "\n";
        return libssh2_channel_write_ex( m_channel, 0, send_data.c_str(), send_data.size() ) == data.size();
        //return true;
    }

string Ssh2::s_password;
    
    void Ssh2::S_KbdCallback(const char *name, int name_len,
                             const char *instruction, int instruction_len,
                             int num_prompts,
                             const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts,
                             LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses,
                             void **abstract)
    {
        (void)name;
        (void)name_len;
        (void)instruction;
        (void)instruction_len;
        if (num_prompts == 1)
        {
            responses[0].text   = strdup(s_password.c_str());
            responses[0].length = (int)s_password.size();
        }
        (void)prompts;
        (void)abstract;
    }
    
    Ssh2::Ssh2(const string &srvIp, int srvPort)
        :m_srvIp(srvIp),m_srvPort(srvPort)
    {
        m_sock = -1;
        m_session = NULL;
        libssh2_init(0);
    }
    
    Ssh2::~Ssh2(void)
    {
        Disconnect();
        libssh2_exit();
    }
    
    bool Ssh2::Connect(const string &userName, const string &userPwd)
    {
        m_sock = socket(AF_INET, SOCK_STREAM, 0);
        
        sockaddr_in sin;
        sin.sin_family = AF_INET;
        sin.sin_port = htons(22);
        sin.sin_addr.s_addr = inet_addr(m_srvIp.c_str());
        if ( connect( m_sock, (sockaddr*)(&sin), sizeof(sockaddr_in) ) != 0 )
        {
            Disconnect();
            return false;
        }
        
        m_session = libssh2_session_init();
        if ( libssh2_session_handshake(m_session, m_sock) )
        {
            Disconnect();
            return false;
        }
        
        int auth_pw = 0;
        string fingerprint = libssh2_hostkey_hash(m_session, LIBSSH2_HOSTKEY_HASH_SHA1);
        string userauthlist = libssh2_userauth_list( m_session, userName.c_str(), (int)userName.size() );
        if ( strstr( userauthlist.c_str(), "password") != NULL )
        {
            auth_pw |= 1;
        }
        if ( strstr( userauthlist.c_str(), "keyboard-interactive") != NULL )
        {
            auth_pw |= 2;
        }
        if ( strstr(userauthlist.c_str(), "publickey") != NULL)
        {
            auth_pw |= 4;
        }
        
        if (auth_pw & 1)
        {
            /* We could authenticate via password */
            if ( libssh2_userauth_password(m_session, userName.c_str(), userPwd.c_str() ) )
            {
                Disconnect();
                return false;
            }
        }
        else if (auth_pw & 2)
        {
            /* Or via keyboard-interactive */
            s_password = userPwd;
            if (libssh2_userauth_keyboard_interactive(m_session, userName.c_str(), &S_KbdCallback) )
            {
                Disconnect();
                return false;
            }
        }
        else
        {
            Disconnect();
            return false;
        }
        
        
        return true;
    }
    
    bool Ssh2::Disconnect(void)
    {
        if( m_session )
        {
            libssh2_session_disconnect(m_session, "Bye bye, Thank you");
            libssh2_session_free(m_session);
            m_session = NULL;
        }
        if( m_sock != -1 )
        {
#ifdef WIN32
            closesocket(m_sock);
#else
            close(m_sock);
#endif
            m_sock = -1;
        }
        return true;
    }
    
    Channel* Ssh2::CreateChannel(const string &ptyType)
    {
        if( NULL == m_session )
        {
            return NULL;
        }
        
        LIBSSH2_CHANNEL *channel = NULL;
        /* Request a shell */
        if ( !(channel = libssh2_channel_open_session(m_session)) )
        {
            return NULL;
        }
        
        /* Request a terminal with 'vanilla' terminal emulation
         * See /etc/termcap for more options
         */
        if ( libssh2_channel_request_pty(channel, ptyType.c_str() ) )
        {
            libssh2_channel_free(channel);
            return NULL;
        }
        
        /* Open a SHELL on that pty */
        if ( libssh2_channel_shell(channel) )
        {
            
            libssh2_channel_free(channel);
            return NULL;
        }
        
        Channel *ret = new Channel(channel);
	cout<<"CreateChannel():"<<endl;
        cout<<ret->Read()<<endl;
	//cout<<"ending"<<endl;
        return ret;
    }

struct node{
	Channel* channel;
	int seq;
	string ip,passwd,username,info;
}hosts[1024];
int total=0;

void readin()
{
	ifstream in("hosts.txt");
    if(!in.is_open())
    {
        cout<<"hosts.txt Error! Fail to open"<<endl;
        exit(1);
    }
	int seq;
	string ip,username,passwd,info;
	while(in>>seq>>ip>>username>>passwd>>info)
	{
		total++;
		hosts[total].seq=seq;
		hosts[total].ip=ip;
		hosts[total].username=username;
		hosts[total].passwd=passwd;
		hosts[total].info=info;
	}
	cout<<">---------Hosts list------------"<<endl;
	for(int i=1;i<=total;i++)
	{
		cout<<hosts[i].seq<<" "<<hosts[i].ip<<" "<<hosts[i].username<<" "<<hosts[i].passwd<<" "<<hosts[i].info<<endl;
	}
	cout<<"<---------Hosts list ends-------"<<endl<<endl;
    in.close();
}

int main(int argc, const char * argv[])
{
   // using namespace std;
   // using namespace fish;
    readin();
    cout<<">--------server info-----------"<<endl; 
    Ssh2 ssh("120.27.250.55");
    ssh.Connect("Aliyun01","Aliyun01");
    Channel* channel = ssh.CreateChannel();
    cout<<"<--------server info ends------"<<endl<<endl;
    string strcmd;
    cout<<">--------remote operation------"<<endl<<"$";
    while(getline(cin,strcmd))
    {
        if(strcmd=="_exit" || strcmd=="_quit")
    	{
    		break;
    	}
    	channel->Write(strcmd);
    	cout<<channel->Read();
    }
    cout<<"---------remote operation ends-------"<<endl;
    delete channel;
    return 0;
}
