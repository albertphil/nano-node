#include <rai/node/node.hpp>

#include <rai/node/common.hpp>
#include <rai/node/rpc.hpp>

#include <future>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_set>

#include <boost/log/expressions.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/network/protocol/http/client.hpp>
#include <boost/program_options.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <ed25519-donna/ed25519.h>

double constexpr rai::node::price_max;
double constexpr rai::node::free_cutoff;
std::chrono::seconds constexpr rai::node::period;
std::chrono::seconds constexpr rai::node::cutoff;
std::chrono::minutes constexpr rai::node::backup_interval;

rai::network::network (boost::asio::io_service & service_a, uint16_t port, rai::node & node_a) :
socket (service_a, boost::asio::ip::udp::endpoint (boost::asio::ip::address_v6::any (), port)),
service (service_a),
resolver (service_a),
node (node_a),
bad_sender_count (0),
on (true),
keepalive_count (0),
publish_count (0),
confirm_req_count (0),
confirm_ack_count (0),
insufficient_work_count (0),
error_count (0)
{
}

void rai::network::receive ()
{
    if (node.config.logging.network_packet_logging ())
    {
        BOOST_LOG (node.log) << "Receiving packet";
    }
    std::unique_lock <std::mutex> lock (socket_mutex);
    socket.async_receive_from (boost::asio::buffer (buffer.data (), buffer.size ()), remote,
        [this] (boost::system::error_code const & error, size_t size_a)
        {
            receive_action (error, size_a);
        });
}

void rai::network::stop ()
{
    on = false;
    socket.close ();
    resolver.cancel ();
}

void rai::network::send_keepalive (rai::endpoint const & endpoint_a)
{
    assert (endpoint_a.address ().is_v6 ());
    rai::keepalive message;
    node.peers.random_fill (message.peers);
    std::shared_ptr <std::vector <uint8_t>> bytes (new std::vector <uint8_t>);
    {
        rai::vectorstream stream (*bytes);
        message.serialize (stream);
    }
    if (node.config.logging.network_keepalive_logging ())
    {
        BOOST_LOG (node.log) << boost::str (boost::format ("Keepalive req sent from %1% to %2%") % endpoint () % endpoint_a);
    }
    auto node_l (node.shared ());
    send_buffer (bytes->data (), bytes->size (), endpoint_a, 0, [bytes, node_l, endpoint_a] (boost::system::error_code const & ec, size_t)
        {
            if (node_l->config.logging.network_logging ())
            {
                if (ec)
                {
                    BOOST_LOG (node_l->log) << boost::str (boost::format ("Error sending keepalive from %1% to %2% %3%") % node_l->network.endpoint () % endpoint_a % ec.message ());
                }
            }
        });
}

void rai::node::keepalive (std::string const & address_a, uint16_t port_a)
{
	auto node_l (shared_from_this ());
	network.resolver.async_resolve (boost::asio::ip::udp::resolver::query (address_a, std::to_string (port_a)), [node_l, address_a, port_a] (boost::system::error_code const & ec, boost::asio::ip::udp::resolver::iterator i_a)
	{
		if (!ec)
		{
			for (auto i (i_a), n (boost::asio::ip::udp::resolver::iterator {}); i != n; ++i)
			{
			    auto endpoint (i->endpoint ());
			    if (endpoint.address ().is_v4 ())
			    {
					endpoint = boost::asio::ip::udp::endpoint (boost::asio::ip::address_v6::v4_mapped (endpoint.address ().to_v4 ()), endpoint.port ());
			    }
				node_l->send_keepalive (endpoint);
			}
		}
		else
		{
			BOOST_LOG (node_l->log) << boost::str (boost::format ("Error resolving address: %1%:%2%, %3%") % address_a % port_a % ec.message ());
		}
	});
}

void rai::network::republish_block (std::unique_ptr <rai::block> block, size_t rebroadcast_a)
{
	auto hash (block->hash ());
    auto list (node.peers.list ());
	// If we're a representative, broadcast a signed confirm, otherwise an unsigned publish
    if (!confirm_broadcast (list, block->clone (), 0, rebroadcast_a))
    {
        rai::publish message (std::move (block));
        std::shared_ptr <std::vector <uint8_t>> bytes (new std::vector <uint8_t>);
        {
            rai::vectorstream stream (*bytes);
            message.serialize (stream);
        }
        auto node_l (node.shared ());
        for (auto i (list.begin ()), n (list.end ()); i != n; ++i)
        {
			if (!node.peers.knows_about (i->endpoint, hash))
			{
				if (node.config.logging.network_publish_logging ())
				{
					BOOST_LOG (node.log) << boost::str (boost::format ("Publish %1% to %2%") % hash.to_string () % i->endpoint);
				}
				send_buffer (bytes->data (), bytes->size (), i->endpoint, rebroadcast_a, [bytes, node_l] (boost::system::error_code const & ec, size_t size)
				{
					if (node_l->config.logging.network_logging ())
					{
						if (ec)
						{
							BOOST_LOG (node_l->log) << boost::str (boost::format ("Error sending publish: %1% from %2%") % ec.message () % node_l->network.endpoint ());
						}
					}
				});
			}
        }
		if (node.config.logging.network_logging ())
		{
			BOOST_LOG (node.log) << boost::str (boost::format ("Block %1% was published from %2%") % hash.to_string () % endpoint ());
		}
    }
	else
	{
		if (node.config.logging.network_logging ())
		{
			BOOST_LOG (node.log) << boost::str (boost::format ("Block %1% was confirmed from %2%") % hash.to_string () % endpoint ());
		}
	}
}

void rai::network::broadcast_confirm_req (rai::block const & block_a)
{
	auto list (node.peers.list ());
	for (auto i (list.begin ()), j (list.end ()); i != j; ++i)
	{
		node.network.send_confirm_req (i->endpoint, block_a);
	}
}

void rai::network::send_confirm_req (boost::asio::ip::udp::endpoint const & endpoint_a, rai::block const & block)
{
    rai::confirm_req message (block.clone ());
    std::shared_ptr <std::vector <uint8_t>> bytes (new std::vector <uint8_t>);
    {
        rai::vectorstream stream (*bytes);
        message.serialize (stream);
    }
    if (node.config.logging.network_logging ())
    {
        BOOST_LOG (node.log) << boost::str (boost::format ("Sending confirm req to %1%") % endpoint_a);
    }
    auto node_l (node.shared ());
    send_buffer (bytes->data (), bytes->size (), endpoint_a, 0, [bytes, node_l] (boost::system::error_code const & ec, size_t size)
        {
            if (node_l->config.logging.network_logging ())
            {
                if (ec)
                {
                    BOOST_LOG (node_l->log) << boost::str (boost::format ("Error sending confirm request: %1%") % ec.message ());
                }
            }
        });
}

namespace
{
class network_message_visitor : public rai::message_visitor
{
public:
    network_message_visitor (rai::node & node_a, rai::endpoint const & sender_a) :
    node (node_a),
    sender (sender_a)
    {
    }
    void keepalive (rai::keepalive const & message_a) override
    {
        if (node.config.logging.network_keepalive_logging ())
        {
            BOOST_LOG (node.log) << boost::str (boost::format ("Received keepalive message from %1%") % sender);
        }
        ++node.network.keepalive_count;
        node.peers.contacted (sender);
        node.network.merge_peers (message_a.peers);
    }
    void publish (rai::publish const & message_a) override
    {
        if (node.config.logging.network_message_logging ())
        {
            BOOST_LOG (node.log) << boost::str (boost::format ("Received publish message from %1%") % sender);
        }
        ++node.network.publish_count;
        node.peers.contacted (sender);
        node.peers.insert (sender, message_a.block->hash ());
        node.process_receive_republish (message_a.block->clone (), 0);
    }
    void confirm_req (rai::confirm_req const & message_a) override
    {
        if (node.config.logging.network_message_logging ())
        {
            BOOST_LOG (node.log) << boost::str (boost::format ("Received confirm_req message from %1%") % sender);
        }
        ++node.network.confirm_req_count;
        node.peers.contacted (sender);
        node.peers.insert (sender, message_a.block->hash ());
        node.process_receive_republish (message_a.block->clone (), 0);
		bool exists;
		{
			rai::transaction transaction (node.store.environment, nullptr, false);
			exists = node.store.block_exists (transaction, message_a.block->hash ());
		}
        if (exists)
        {
            node.process_confirmation (*message_a.block, sender);
        }
    }
    void confirm_ack (rai::confirm_ack const & message_a) override
    {
        if (node.config.logging.network_message_logging ())
        {
            BOOST_LOG (node.log) << boost::str (boost::format ("Received confirm_ack message from %1%") % sender);
        }
        ++node.network.confirm_ack_count;
        node.peers.contacted (sender);
        node.peers.insert (sender, message_a.vote.block->hash ());
        node.process_receive_republish (message_a.vote.block->clone (), 0);
        node.vote (message_a.vote);
    }
    void bulk_pull (rai::bulk_pull const &) override
    {
        assert (false);
    }
    void bulk_push (rai::bulk_push const &) override
    {
        assert (false);
    }
    void frontier_req (rai::frontier_req const &) override
    {
        assert (false);
    }
    rai::node & node;
    rai::endpoint sender;
};
}

void rai::network::receive_action (boost::system::error_code const & error, size_t size_a)
{
    if (!error && on)
    {
        if (!rai::reserved_address (remote) && remote != endpoint ())
        {
            network_message_visitor visitor (node, remote);
            rai::message_parser parser (visitor, node.work);
            parser.deserialize_buffer (buffer.data (), size_a);
            if (parser.error)
            {
                ++error_count;
            }
            else if (parser.insufficient_work)
            {
                if (node.config.logging.insufficient_work_logging ())
                {
                    BOOST_LOG (node.log) << "Insufficient work in message";
                }
                ++insufficient_work_count;
            }
        }
        else
        {
            if (node.config.logging.network_logging ())
            {
                BOOST_LOG (node.log) << boost::str (boost::format ("Reserved sender %1%") % remote.address ().to_string ());
            }
            ++bad_sender_count;
        }
        receive ();
    }
    else
    {
        if (node.config.logging.network_logging ())
        {
            BOOST_LOG (node.log) << boost::str (boost::format ("Receive error: %1%") % error.message ());
        }
        node.alarm.add (std::chrono::system_clock::now () + std::chrono::seconds (5), [this] () { receive (); });
    }
}

// Send keepalives to all the peers we've been notified of
void rai::network::merge_peers (std::array <rai::endpoint, 8> const & peers_a)
{
    for (auto i (peers_a.begin ()), j (peers_a.end ()); i != j; ++i)
    {
        if (!node.peers.not_a_peer (*i) && !node.peers.known_peer (*i))
        {
            send_keepalive (*i);
        }
    }
}

bool rai::operation::operator > (rai::operation const & other_a) const
{
    return wakeup > other_a.wakeup;
}

rai::alarm::alarm (boost::asio::io_service & service_a) :
service (service_a),
thread ([this] () { run (); })
{
}

rai::alarm::~alarm ()
{
	add (std::chrono::system_clock::now (), nullptr);
	thread.join ();
}

void rai::alarm::run ()
{
    std::unique_lock <std::mutex> lock (mutex);
	auto done (false);
    while (!done)
    {
        if (!operations.empty ())
        {
            auto & operation (operations.top ());
			if (operation.function)
			{
				if (operation.wakeup <= std::chrono::system_clock::now ())
				{
					service.post (operation.function);
					operations.pop ();
				}
				else
				{
					auto wakeup (operation.wakeup);
					condition.wait_until (lock, wakeup);
				}
			}
			else
			{
				done = true;
			}
        }
        else
        {
            condition.wait (lock);
        }
    }
}

void rai::alarm::add (std::chrono::system_clock::time_point const & wakeup_a, std::function <void ()> const & operation)
{
    std::lock_guard <std::mutex> lock (mutex);
	operations.push (rai::operation ({wakeup_a, operation}));
	condition.notify_all ();
}

rai::logging::logging () :
ledger_logging_value (false),
ledger_duplicate_logging_value (false),
network_logging_value (true),
network_message_logging_value (false),
network_publish_logging_value (false),
network_packet_logging_value (false),
network_keepalive_logging_value (false),
node_lifetime_tracing_value (false),
insufficient_work_logging_value (true),
log_rpc_value (true),
bulk_pull_logging_value (false),
work_generation_time_value (true),
log_to_cerr_value (false),
max_size (16 * 1024 * 1024)
{
}

void rai::logging::serialize_json (boost::property_tree::ptree & tree_a) const
{
	tree_a.put ("ledger", ledger_logging_value);
	tree_a.put ("ledger_duplicate", ledger_duplicate_logging_value);
	tree_a.put ("network", network_logging_value);
	tree_a.put ("network_message", network_message_logging_value);
	tree_a.put ("network_publish", network_publish_logging_value);
	tree_a.put ("network_packet", network_packet_logging_value);
	tree_a.put ("network_keepalive", network_keepalive_logging_value);
	tree_a.put ("node_lifetime_tracing", node_lifetime_tracing_value);
	tree_a.put ("insufficient_work", insufficient_work_logging_value);
	tree_a.put ("log_rpc", log_rpc_value);
	tree_a.put ("bulk_pull", bulk_pull_logging_value);
	tree_a.put ("work_generation_time", work_generation_time_value);
	tree_a.put ("log_to_cerr", log_to_cerr_value);
	tree_a.put ("max_size", max_size);
}

bool rai::logging::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto result (false);
	try
	{
		ledger_logging_value = tree_a.get <bool> ("ledger");
		ledger_duplicate_logging_value = tree_a.get <bool> ("ledger_duplicate");
		network_logging_value = tree_a.get <bool> ("network");
		network_message_logging_value = tree_a.get <bool> ("network_message");
		network_publish_logging_value = tree_a.get <bool> ("network_publish");
		network_packet_logging_value = tree_a.get <bool> ("network_packet");
		network_keepalive_logging_value = tree_a.get <bool> ("network_keepalive");
		node_lifetime_tracing_value = tree_a.get <bool> ("node_lifetime_tracing");
		insufficient_work_logging_value = tree_a.get <bool> ("insufficient_work");
		log_rpc_value = tree_a.get <bool> ("log_rpc");
		bulk_pull_logging_value = tree_a.get <bool> ("bulk_pull");
		work_generation_time_value = tree_a.get <bool> ("work_generation_time");
		log_to_cerr_value = tree_a.get <bool> ("log_to_cerr");
		max_size = tree_a.get <uintmax_t> ("max_size");
	}
	catch (std::runtime_error const &)
	{
		result = true;
	}
	return result;
}

bool rai::logging::ledger_logging () const
{
	return ledger_logging_value;
}

bool rai::logging::ledger_duplicate_logging () const
{
	return ledger_logging () && ledger_duplicate_logging_value;
}

bool rai::logging::network_logging () const
{
	return network_logging_value;
}

bool rai::logging::network_message_logging () const
{
	return network_logging () && network_message_logging_value;
}

bool rai::logging::network_publish_logging () const
{
	return network_logging () && network_publish_logging_value;
}

bool rai::logging::network_packet_logging () const
{
	return network_logging () && network_packet_logging_value;
}

bool rai::logging::network_keepalive_logging () const
{
	return network_logging () && network_keepalive_logging_value;
}

bool rai::logging::node_lifetime_tracing () const
{
	return node_lifetime_tracing_value;
}

bool rai::logging::insufficient_work_logging () const
{
	return network_logging () && insufficient_work_logging_value;
}

bool rai::logging::log_rpc () const
{
	return network_logging () && log_rpc_value;
}

bool rai::logging::bulk_pull_logging () const
{
	return network_logging () && bulk_pull_logging_value;
}

bool rai::logging::work_generation_time () const
{
	return work_generation_time_value;
}

bool rai::logging::log_to_cerr () const
{
	return log_to_cerr_value;
}

rai::node_init::node_init () :
block_store_init (false),
wallet_init (false)
{
}

bool rai::node_init::error ()
{
    return block_store_init || wallet_init;
}

namespace {
class send_visitor : public rai::block_visitor
{
public:
	send_visitor (rai::node & node_a) :
	node (node_a)
	{
	}
	void send_block (rai::send_block const & block_a)
	{
		auto receive (false);
		{
			rai::transaction transaction (node.store.environment, nullptr, false);
			for (auto i (node.wallets.items.begin ()), n (node.wallets.items.end ()); i != n && receive == false; ++i)
			{
				auto & wallet (*i->second);
				if (wallet.store.find (transaction, block_a.hashables.destination) != wallet.store.end ())
				{
					receive = true;
				}
			}
		}
		if (receive)
		{
			if (node.config.logging.ledger_logging ())
			{
				BOOST_LOG (node.log) << boost::str (boost::format ("Starting fast confirmation of block: %1%") % block_a.hash ().to_string ());
			}
			auto node_l (node.shared ());
			node.active.start (block_a, [node_l] (rai::block & block_a)
			{
				node_l->process_confirmed (block_a);
			});
        }
	}
	void receive_block (rai::receive_block const &)
	{
	}
	void open_block (rai::open_block const &)
	{
	}
	void change_block (rai::change_block const &)
	{
	}
	rai::node & node;
};
}

rai::node_config::node_config () :
node_config (rai::network::node_port, rai::logging ())
{
}

rai::node_config::node_config (uint16_t peering_port_a, rai::logging const & logging_a) :
peering_port (peering_port_a),
logging (logging_a),
packet_delay_microseconds (5000),
bootstrap_fraction_numerator (1),
creation_rebroadcast (2),
rebroadcast_delay (15),
receive_minimum (rai::Mrai_ratio),
inactive_supply (0),
password_fanout (1024),
io_threads (std::max <unsigned> (4, std::thread::hardware_concurrency ())),
work_threads (std::max <unsigned> (4, std::thread::hardware_concurrency ()))
{
	switch (rai::rai_network)
	{
		case rai::rai_networks::rai_test_network:
			preconfigured_representatives.push_back (rai::genesis_account);
			break;
		case rai::rai_networks::rai_beta_network:
			preconfigured_peers.push_back ("rai.raiblocks.net");
			preconfigured_representatives.push_back (rai::account ("59750C057F42806F40C5D9EAA1E0263E9DB48FE385BD0172BFC573BD37EEC4A7"));
			preconfigured_representatives.push_back (rai::account ("8B05C9B160DE9B006FA27DD6A368D7CA122A2EE7537C308CF22EFD3ABF5B36C3"));
			preconfigured_representatives.push_back (rai::account ("91D51BF05F02698EBB4649FB06D1BBFD2E4AE2579660E8D784A002D9C0CB1BD2"));
			preconfigured_representatives.push_back (rai::account ("CB35ED23D47E1A16667EDE415CD4CD05961481D7D23A43958FAE81FC12FA49FF"));
			break;
		case rai::rai_networks::rai_live_network:
			preconfigured_peers.push_back ("rai.raiblocks.net");
			preconfigured_representatives.push_back (rai::account ("A30E0A32ED41C8607AA9212843392E853FCBCB4E7CB194E35C94F07F91DE59EF"));
			preconfigured_representatives.push_back (rai::account ("67556D31DDFC2A440BF6147501449B4CB9572278D034EE686A6BEE29851681DF"));
			preconfigured_representatives.push_back (rai::account ("5C2FBB148E006A8E8BA7A75DD86C9FE00C83F5FFDBFD76EAA09531071436B6AF"));
			preconfigured_representatives.push_back (rai::account ("AE7AC63990DAAAF2A69BF11C913B928844BF5012355456F2F164166464024B29"));
			preconfigured_representatives.push_back (rai::account ("BD6267D6ECD8038327D2BCC0850BDF8F56EC0414912207E81BCF90DFAC8A4AAA"));
			preconfigured_representatives.push_back (rai::account ("2399A083C600AA0572F5E36247D978FCFC840405F8D4B6D33161C0066A55F431"));
			preconfigured_representatives.push_back (rai::account ("2298FAB7C61058E77EA554CB93EDEEDA0692CBFCC540AB213B2836B29029E23A"));
			preconfigured_representatives.push_back (rai::account ("3FE80B4BC842E82C1C18ABFEEC47EA989E63953BC82AC411F304D13833D52A56"));
			break;
		default:
			assert (false);
			break;
	}
}

void rai::node_config::serialize_json (boost::property_tree::ptree & tree_a) const
{
	tree_a.put ("version", "3");
	tree_a.put ("peering_port", std::to_string (peering_port));
	tree_a.put ("packet_delay_microseconds", std::to_string (packet_delay_microseconds));
	tree_a.put ("bootstrap_fraction_numerator", std::to_string (bootstrap_fraction_numerator));
	tree_a.put ("creation_rebroadcast", std::to_string (creation_rebroadcast));
	tree_a.put ("rebroadcast_delay", std::to_string (rebroadcast_delay));
	tree_a.put ("receive_minimum", receive_minimum.to_string_dec ());
	boost::property_tree::ptree logging_l;
	logging.serialize_json (logging_l);
	tree_a.add_child ("logging", logging_l);
	boost::property_tree::ptree work_peers_l;
	for (auto i (work_peers.begin ()), n (work_peers.end ()); i != n; ++i)
	{
		boost::property_tree::ptree entry;
		entry.put ("", boost::str (boost::format ("%1%:%2%") % i->first % i->second));
		work_peers_l.push_back (std::make_pair ("", entry));
	}
	tree_a.add_child ("work_peers", work_peers_l);
	boost::property_tree::ptree preconfigured_peers_l;
	for (auto i (preconfigured_peers.begin ()), n (preconfigured_peers.end ()); i != n; ++i)
	{
		boost::property_tree::ptree entry;
		entry.put ("", *i);
		preconfigured_peers_l.push_back (std::make_pair ("", entry));
	}
	tree_a.add_child ("preconfigured_peers", preconfigured_peers_l);
	boost::property_tree::ptree preconfigured_representatives_l;
	for (auto i (preconfigured_representatives.begin ()), n (preconfigured_representatives.end ()); i != n; ++i)
	{
		boost::property_tree::ptree entry;
		entry.put ("", i->to_account ());
		preconfigured_representatives_l.push_back (std::make_pair ("", entry));
	}
	tree_a.add_child ("preconfigured_representatives", preconfigured_representatives_l);
	tree_a.put ("inactive_supply", inactive_supply.to_string_dec ());
	tree_a.put ("password_fanout", std::to_string (password_fanout));
	tree_a.put ("io_threads", std::to_string (io_threads));
	tree_a.put ("work_threads", std::to_string (work_threads));
}

bool rai::node_config::upgrade_json (unsigned version, boost::property_tree::ptree & tree_a)
{
	auto result (false);
	switch (version)
	{
	case 1:
	{
		auto reps_l (tree_a.get_child ("preconfigured_representatives"));
		boost::property_tree::ptree reps;
		for (auto i (reps_l.begin ()), n (reps_l.end ()); i != n; ++i)
		{
			rai::uint256_union account;
			account.decode_account (i->second.get <std::string> (""));
			boost::property_tree::ptree entry;
			entry.put ("", account.to_account ());
			reps.push_back (std::make_pair ("", entry));
		}
		tree_a.erase ("preconfigured_representatives");
		tree_a.add_child ("preconfigured_representatives", reps);
		tree_a.erase ("version");
		tree_a.put ("version", "2");
		result = true;
	}
	case 2:
	{
		tree_a.put ("inactive_supply", rai::uint128_union (0).to_string_dec ());
		tree_a.put ("password_fanout", std::to_string (1024));
		tree_a.put ("io_threads", std::to_string (io_threads));
		tree_a.put ("work_threads", std::to_string (work_threads));
		tree_a.erase ("version");
		tree_a.put ("version", "3");
		result = true;
	}
	case 3:
		break;
	break;
	default:
		throw std::runtime_error ("Unknown node_config version");
	}
	return result;
}

bool rai::node_config::deserialize_json (bool & upgraded_a, boost::property_tree::ptree & tree_a)
{
	auto result (false);
	try
	{
		auto version_l (tree_a.get_optional <std::string> ("version"));
		if (!version_l)
		{
			tree_a.put ("version", "1");
			version_l = "1";
			auto work_peers_l (tree_a.get_child_optional ("work_peers"));
			if (!work_peers_l)
			{
				tree_a.add_child ("work_peers", boost::property_tree::ptree ());
			}
			upgraded_a = true;
		}
		upgraded_a |= upgrade_json (std::stoull (version_l.get ()), tree_a);
		auto peering_port_l (tree_a.get <std::string> ("peering_port"));
		auto packet_delay_microseconds_l (tree_a.get <std::string> ("packet_delay_microseconds"));
		auto bootstrap_fraction_numerator_l (tree_a.get <std::string> ("bootstrap_fraction_numerator"));
		auto creation_rebroadcast_l (tree_a.get <std::string> ("creation_rebroadcast"));
		auto rebroadcast_delay_l (tree_a.get <std::string> ("rebroadcast_delay"));
		auto receive_minimum_l (tree_a.get <std::string> ("receive_minimum"));
		auto logging_l (tree_a.get_child ("logging"));
		work_peers.clear ();
		auto work_peers_l (tree_a.get_child ("work_peers"));
		for (auto i (work_peers_l.begin ()), n (work_peers_l.end ()); i != n; ++i)
		{
			auto work_peer (i->second.get <std::string> (""));
			boost::asio::ip::address address;
			uint16_t port;
			result |= rai::parse_address_port (work_peer, address, port);
			if (!result)
			{
				work_peers.push_back (std::make_pair (address, port));
			}
		}
		auto preconfigured_peers_l (tree_a.get_child ("preconfigured_peers"));
		preconfigured_peers.clear ();
		for (auto i (preconfigured_peers_l.begin ()), n (preconfigured_peers_l.end ()); i != n; ++i)
		{
			auto bootstrap_peer (i->second.get <std::string> (""));
			preconfigured_peers.push_back (bootstrap_peer);
		}
		auto preconfigured_representatives_l (tree_a.get_child ("preconfigured_representatives"));
		preconfigured_representatives.clear ();
		for (auto i (preconfigured_representatives_l.begin ()), n (preconfigured_representatives_l.end ()); i != n; ++i)
		{
			rai::account representative (0);
			result = result || representative.decode_account (i->second.get <std::string> (""));
			preconfigured_representatives.push_back (representative);
		}
		if (preconfigured_representatives.empty ())
		{
			result = true;
		}
		auto inactive_supply_l (tree_a.get <std::string> ("inactive_supply"));
		auto password_fanout_l (tree_a.get <std::string> ("password_fanout"));
		auto io_threads_l (tree_a.get <std::string> ("io_threads"));
		auto work_threads_l (tree_a.get <std::string> ("work_threads"));
		try
		{
			peering_port = std::stoul (peering_port_l);
			packet_delay_microseconds = std::stoul (packet_delay_microseconds_l);
			bootstrap_fraction_numerator = std::stoul (bootstrap_fraction_numerator_l);
			creation_rebroadcast = std::stoul (creation_rebroadcast_l);
			rebroadcast_delay = std::stoul (rebroadcast_delay_l);
			password_fanout = std::stoul (password_fanout_l);
			io_threads = std::stoul (io_threads_l);
			work_threads = std::stoul (work_threads_l);
			result |= creation_rebroadcast > 10;
			result |= rebroadcast_delay > 300;
			result |= peering_port > std::numeric_limits <uint16_t>::max ();
			result |= logging.deserialize_json (logging_l);
			result |= receive_minimum.decode_dec (receive_minimum_l);
			result |= inactive_supply.decode_dec (inactive_supply_l);
			result |= password_fanout < 16;
			result |= password_fanout > 1024 * 1024;
			result |= io_threads == 0;
			result |= work_threads == 0;
		}
		catch (std::logic_error const &)
		{
			result = true;
		}
	}
	catch (std::runtime_error const &)
	{
		result = true;
	}
	return result;
}

rai::account rai::node_config::random_representative ()
{
	assert (preconfigured_representatives.size () > 0);
	size_t index (rai::random_pool.GenerateWord32 (0, preconfigured_representatives.size () - 1));
	auto result (preconfigured_representatives [index]);
	return result;
}

void rai::node_observers::add_blocks (std::function <void (rai::block const &, rai::account const &, rai::amount const &)> const & observer_a)
{
	std::lock_guard <std::mutex> lock (mutex);
	blocks.push_back (observer_a);
}

void rai::node_observers::add_wallet (std::function <void (rai::account const &, bool)> const & observer_a)
{
	std::lock_guard <std::mutex> lock (mutex);
	wallet.push_back (observer_a);
}

void rai::node_observers::add_vote (std::function <void (rai::vote const &)> const & observer_a)
{
	std::lock_guard <std::mutex> lock (mutex);
	vote.push_back (observer_a);
}

void rai::node_observers::add_endpoint (std::function <void (rai::endpoint const &)> const & observer_a)
{
	std::lock_guard <std::mutex> lock (mutex);
	endpoint.push_back (observer_a);
}

void rai::node_observers::add_disconnect (std::function <void ()> const & observer_a)
{
	std::lock_guard <std::mutex> lock (mutex);
	disconnect.push_back (observer_a);
}

void rai::node_observers::call_blocks (rai::block const & block_a, rai::account const & account_a, rai::amount const & amount_a)
{
	std::lock_guard <std::mutex> lock (mutex);
	for (auto & i: blocks)
	{
		i (block_a, account_a, amount_a);
	}
}

void rai::node_observers::call_wallet (rai::account const & account_a, bool active_a)
{
	std::lock_guard <std::mutex> lock (mutex);
	for (auto & i: wallet)
	{
		i (account_a, active_a);
	}
}

void rai::node_observers::call_vote (rai::vote const & vote_a)
{
	std::lock_guard <std::mutex> lock (mutex);
	for (auto & i: vote)
	{
		i (vote_a);
	}
}

void rai::node_observers::call_endpoint (rai::endpoint const & endpoint_a)
{
	std::lock_guard <std::mutex> lock (mutex);
	for (auto & i: endpoint)
	{
		i (endpoint_a);
	}
}

void rai::node_observers::call_disconnect ()
{
	std::lock_guard <std::mutex> lock (mutex);
	for (auto & i: disconnect)
	{
		i ();
	}
}

rai::node::node (rai::node_init & init_a, boost::asio::io_service & service_a, uint16_t peering_port_a, boost::filesystem::path const & application_path_a, rai::alarm & alarm_a, rai::logging const & logging_a, rai::work_pool & work_a) :
node (init_a, service_a, application_path_a, alarm_a, rai::node_config (peering_port_a, logging_a), work_a)
{
}

rai::node::node (rai::node_init & init_a, boost::asio::io_service & service_a, boost::filesystem::path const & application_path_a, rai::alarm & alarm_a, rai::node_config const & config_a, rai::work_pool & work_a) :
config (config_a),
alarm (alarm_a),
work (work_a),
store (init_a.block_store_init, application_path_a / "data.ldb"),
gap_cache (*this),
ledger (store, config_a.inactive_supply.number ()),
active (*this),
wallets (init_a.block_store_init, *this),
network (service_a, config.peering_port, *this),
bootstrap_initiator (*this),
bootstrap (service_a, config.peering_port, *this),
peers (network.endpoint ()),
application_path (application_path_a)
{
	wallets.observer = [this] (rai::account const & account_a, bool active)
	{
		observers.call_wallet (account_a, active);
	};
	peers.peer_observer = [this] (rai::endpoint const & endpoint_a)
	{
		observers.call_endpoint (endpoint_a);
	};
	peers.disconnect_observer = [this] ()
	{
		observers.call_disconnect ();
	};
	observers.add_endpoint ([this] (rai::endpoint const & endpoint_a)
	{
		this->network.send_keepalive (endpoint_a);
		this->bootstrap_initiator.warmup (endpoint_a);
	});
    observers.add_vote ([this] (rai::vote const & vote_a)
    {
        active.vote (vote_a);
    });
    observers.add_vote ([this] (rai::vote const & vote_a)
    {
		rai::transaction transaction (store.environment, nullptr, false);
		this->gap_cache.vote (transaction, vote_a);
    });
    if (config.logging.log_to_cerr ())
    {
        boost::log::add_console_log (std::cerr, boost::log::keywords::format = "[%TimeStamp%]: %Message%");
    }
    boost::log::add_common_attributes ();
	boost::log::add_file_log (boost::log::keywords::target = application_path_a / "log", boost::log::keywords::file_name = application_path_a / "log" / "log_%Y-%m-%d_%H-%M-%S.%N.log", boost::log::keywords::rotation_size = 4 * 1024 * 1024, boost::log::keywords::auto_flush = true, boost::log::keywords::scan_method = boost::log::sinks::file::scan_method::scan_matching, boost::log::keywords::max_size = config.logging.max_size, boost::log::keywords::format = "[%TimeStamp%]: %Message%");
	BOOST_LOG (log) << "Node starting, version: " << RAIBLOCKS_VERSION_MAJOR << "." << RAIBLOCKS_VERSION_MINOR << "." << RAIBLOCKS_VERSION_PATCH;
	BOOST_LOG (log) << boost::str (boost::format ("Work pool running %1% threads") % work.threads.size ());
	observers.add_blocks ([this] (rai::block const & block_a, rai::account const & account_a, rai::amount const &)
    {
		send_visitor visitor (*this);
		block_a.visit (visitor);
    });
    if (!init_a.error ())
    {
        if (config.logging.node_lifetime_tracing ())
        {
            std::cerr << "Constructing node\n";
        }
		rai::transaction transaction (store.environment, nullptr, true);
        if (store.latest_begin (transaction) == store.latest_end ())
        {
            // Store was empty meaning we just created it, add the genesis block
            rai::genesis genesis;
            genesis.initialize (transaction, store);
        }
    }
}

rai::node::~node ()
{
    if (config.logging.node_lifetime_tracing ())
    {
        std::cerr << "Destructing node\n";
    }
}

void rai::node::send_keepalive (rai::endpoint const & endpoint_a)
{
    auto endpoint_l (endpoint_a);
    if (endpoint_l.address ().is_v4 ())
    {
        endpoint_l = rai::endpoint (boost::asio::ip::address_v6::v4_mapped (endpoint_l.address ().to_v4 ()), endpoint_l.port ());
    }
    assert (endpoint_l.address ().is_v6 ());
    network.send_keepalive (endpoint_l);
}

void rai::node::vote (rai::vote const & vote_a)
{
	observers.call_vote (vote_a);
}

rai::gap_cache::gap_cache (rai::node & node_a) :
node (node_a)
{
}

void rai::gap_cache::add (rai::block const & block_a, rai::block_hash needed_a)
{
	auto hash (block_a.hash ());
    std::lock_guard <std::mutex> lock (mutex);
    auto existing (blocks.get <2>().find (hash));
    if (existing != blocks.get <2> ().end ())
    {
        blocks.get <2> ().modify (existing, [&block_a] (rai::gap_information & info)
		{
			info.arrival = std::chrono::system_clock::now ();
		});
    }
    else
    {
		blocks.insert ({std::chrono::system_clock::now (), needed_a, hash, std::unique_ptr <rai::votes> (new rai::votes (block_a)), block_a.clone ()});
        if (blocks.size () > max)
        {
            blocks.get <1> ().erase (blocks.get <1> ().begin ());
        }
    }
}

std::vector <std::unique_ptr <rai::block>> rai::gap_cache::get (rai::block_hash const & hash_a)
{
    std::lock_guard <std::mutex> lock (mutex);
    std::vector <std::unique_ptr <rai::block>> result;
    for (auto i (blocks.find (hash_a)), n (blocks.end ()); i != n && i->required == hash_a; ++i)
    {
        blocks.modify (i, [&result] (rai::gap_information & info)
		{
			result.push_back (std::move (info.block));
		});
    }
	blocks.erase (hash_a);
    return result;
}

void rai::gap_cache::vote (MDB_txn * transaction_a, rai::vote const & vote_a)
{
    std::lock_guard <std::mutex> lock (mutex);
    auto hash (vote_a.block->hash ());
    auto existing (blocks.get <2> ().find (hash));
    if (existing != blocks.get <2> ().end ())
    {
        auto changed (existing->votes->vote (vote_a));
        if (changed)
        {
            auto winner (node.ledger.winner (transaction_a, *existing->votes));
            if (winner.first > bootstrap_threshold (transaction_a))
            {
				auto node_l (node.shared ());
				auto now (std::chrono::system_clock::now ());
				node.alarm.add (rai::rai_network == rai::rai_networks::rai_test_network ? now + std::chrono::milliseconds (10) : now + std::chrono::seconds (5), [node_l, hash] ()
				{
					rai::transaction transaction (node_l->store.environment, nullptr, false);
					if (!node_l->store.block_exists (transaction, hash))
					{
						BOOST_LOG (node_l->log) << boost::str (boost::format ("Missing confirmed block %1%") % hash.to_string ());
						node_l->bootstrap_initiator.bootstrap_any ();
					}
					else
					{
						BOOST_LOG (node_l->log) << boost::str (boost::format ("Block: %1% was inserted while voting") % hash.to_string ());
					}
				});
            }
        }
    }
}

rai::uint128_t rai::gap_cache::bootstrap_threshold (MDB_txn * transaction_a)
{
    auto result ((node.ledger.supply (transaction_a) / 256) * node.config.bootstrap_fraction_numerator);
	return result;
}

bool rai::network::confirm_broadcast (std::vector <rai::peer_information> & list_a, std::unique_ptr <rai::block> block_a, uint64_t sequence_a, size_t rebroadcast_a)
{
    bool result (false);
	node.wallets.foreach_representative ([&result, &block_a, &list_a, this, sequence_a, rebroadcast_a] (rai::public_key const & pub_a, rai::raw_key const & prv_a)
	{
		auto hash (block_a->hash ());
		for (auto j (list_a.begin ()), m (list_a.end ()); j != m; ++j)
		{
			if (!node.peers.knows_about (j->endpoint, hash))
			{
				confirm_block (prv_a, pub_a, block_a->clone (), sequence_a, j->endpoint, rebroadcast_a);
				result = true;
			}
		}
	});
    return result;
}

void rai::network::confirm_block (rai::raw_key const & prv, rai::public_key const & pub, std::unique_ptr <rai::block> block_a, uint64_t sequence_a, rai::endpoint const & endpoint_a, size_t rebroadcast_a)
{
    rai::confirm_ack confirm (pub, prv, sequence_a, std::move (block_a));
    std::shared_ptr <std::vector <uint8_t>> bytes (new std::vector <uint8_t>);
    {
        rai::vectorstream stream (*bytes);
        confirm.serialize (stream);
    }
    if (node.config.logging.network_publish_logging ())
    {
        BOOST_LOG (node.log) << boost::str (boost::format ("Sending confirm_ack for block %1% to %2%") % confirm.vote.block->hash ().to_string () % endpoint_a);
    }
    auto node_l (node.shared ());
    node.network.send_buffer (bytes->data (), bytes->size (), endpoint_a, 0, [bytes, node_l, endpoint_a] (boost::system::error_code const & ec, size_t size_a)
	{
		if (node_l->config.logging.network_logging ())
		{
			if (ec)
			{
				BOOST_LOG (node_l->log) << boost::str (boost::format ("Error broadcasting confirm_ack to %1%: %2%") % endpoint_a % ec.message ());
			}
		}
	});
}

void rai::node::process_receive_republish (std::unique_ptr <rai::block> incoming, size_t rebroadcast_a)
{
	std::vector <std::tuple <rai::process_return, std::unique_ptr <rai::block>>> completed;
	{
		rai::transaction transaction (store.environment, nullptr, true);
		assert (incoming != nullptr);
		process_receive_many (transaction, *incoming, [this, rebroadcast_a, &completed] (rai::process_return result_a, rai::block const & block_a)
		{
			switch (result_a.code)
			{
				case rai::process_result::progress:
				{
					completed.push_back (std::make_tuple (result_a, block_a.clone ()));
					this->network.republish_block (block_a.clone (), rebroadcast_a);
					break;
				}
				default:
				{
					break;
				}
			}
		});
	}
	for (auto & i: completed)
	{
		observers.call_blocks (*std::get <1> (i), std::get <0> (i).account, std::get <0>(i).amount);
	}
}

void rai::node::process_receive_many (rai::transaction & transaction_a, rai::block const & block_a, std::function <void (rai::process_return, rai::block const &)> completed_a)
{
	std::vector <std::unique_ptr <rai::block>> blocks;
	blocks.push_back (block_a.clone ());
    while (!blocks.empty ())
    {
		auto block (std::move (blocks.back ()));
		blocks.pop_back ();
        auto hash (block->hash ());
        auto process_result (process_receive_one (transaction_a, *block));
		completed_a (process_result, *block);
		auto cached (gap_cache.get (hash));
		blocks.resize (blocks.size () + cached.size ());
		std::move (cached.begin (), cached.end (), blocks.end () - cached.size ());
    }
}

rai::process_return rai::node::process_receive_one (rai::transaction & transaction_a, rai::block const & block_a)
{
	rai::process_return result;
	result = ledger.process (transaction_a, block_a);
    switch (result.code)
    {
        case rai::process_result::progress:
        {
            if (config.logging.ledger_logging ())
            {
                std::string block;
                block_a.serialize_json (block);
                BOOST_LOG (log) << boost::str (boost::format ("Processing block %1% %2%") % block_a.hash ().to_string () % block);
            }
            break;
        }
        case rai::process_result::gap_previous:
        {
            if (config.logging.ledger_logging ())
            {
                BOOST_LOG (log) << boost::str (boost::format ("Gap previous for: %1%") % block_a.hash ().to_string ());
            }
            auto previous (block_a.previous ());
            gap_cache.add (block_a, previous);
            break;
        }
        case rai::process_result::gap_source:
        {
            if (config.logging.ledger_logging ())
            {
                BOOST_LOG (log) << boost::str (boost::format ("Gap source for: %1%") % block_a.hash ().to_string ());
            }
            auto source (block_a.source ());
            gap_cache.add (block_a, source);
            break;
        }
        case rai::process_result::old:
        {
			{
				auto root (block_a.root ());
				auto hash (block_a.hash ());
				auto existing (store.block_get (transaction_a, hash));
				if (existing != nullptr)
				{
					// Replace block with one that has higher work value
					if (work.work_value (root, block_a.block_work ()) > work.work_value (root, existing->block_work ()))
					{
						store.block_put (transaction_a, hash, block_a);
					}
				}
				else
				{
					// Could have been rolled back, maybe
				}
			}
            if (config.logging.ledger_duplicate_logging ())
            {
                BOOST_LOG (log) << boost::str (boost::format ("Old for: %1%") % block_a.hash ().to_string ());
            }
            break;
        }
        case rai::process_result::bad_signature:
        {
            if (config.logging.ledger_logging ())
            {
                BOOST_LOG (log) << boost::str (boost::format ("Bad signature for: %1%") % block_a.hash ().to_string ());
            }
            break;
        }
        case rai::process_result::overspend:
        {
            if (config.logging.ledger_logging ())
            {
                BOOST_LOG (log) << boost::str (boost::format ("Overspend for: %1%") % block_a.hash ().to_string ());
            }
            break;
        }
        case rai::process_result::unreceivable:
        {
            if (config.logging.ledger_logging ())
            {
                BOOST_LOG (log) << boost::str (boost::format ("Unreceivable for: %1%") % block_a.hash ().to_string ());
            }
            break;
        }
        case rai::process_result::not_receive_from_send:
        {
            if (config.logging.ledger_logging ())
            {
                BOOST_LOG (log) << boost::str (boost::format ("Not receive from send for: %1%") % block_a.hash ().to_string ());
            }
            break;
        }
        case rai::process_result::fork:
        {
            if (config.logging.ledger_logging ())
            {
                BOOST_LOG (log) << boost::str (boost::format ("Fork for: %1%") % block_a.hash ().to_string ());
            }
			std::unique_ptr <rai::block> root;
			root = ledger.successor (transaction_a, block_a.root ());
			auto node_l (shared_from_this ());
			active.start (*root, [node_l] (rai::block & block_a)
			{
				node_l->process_confirmed (block_a);
			});
            break;
        }
        case rai::process_result::account_mismatch:
        {
            if (config.logging.ledger_logging ())
            {
                BOOST_LOG (log) << boost::str (boost::format ("Account mismatch for: %1%") % block_a.hash ().to_string ());
            }
        }
    }
    return result;
}

rai::process_return rai::node::process (rai::block const & block_a)
{
	rai::transaction transaction (store.environment, nullptr, true);
	auto result (ledger.process (transaction, block_a));
	return result;
}

std::vector <rai::peer_information> rai::peer_container::list ()
{
    std::vector <rai::peer_information> result;
    std::lock_guard <std::mutex> lock (mutex);
    result.reserve (peers.size ());
    for (auto i (peers.begin ()), j (peers.end ()); i != j; ++i)
    {
        result.push_back (*i);
    }
    return result;
}

std::vector <rai::peer_information> rai::peer_container::bootstrap_candidates ()
{
    std::vector <rai::peer_information> result;
    std::lock_guard <std::mutex> lock (mutex);
	auto now (std::chrono::system_clock::now ());
    for (auto i (peers.begin ()), j (peers.end ()); i != j; ++i)
    {
		if (now - i->last_bootstrap_failure > std::chrono::minutes (15))
		{
			result.push_back (*i);
		}
    }
    return result;
}
void rai::node::process_confirmation (rai::block const & block_a, rai::endpoint const & sender)
{
	wallets.foreach_representative ([this, &block_a, &sender] (rai::public_key const & pub_a, rai::raw_key const & prv_a)
	{
		if (config.logging.network_message_logging ())
		{
			BOOST_LOG (log) << boost::str (boost::format ("Sending confirm ack to: %1%") % sender);
		}
		this->network.confirm_block (prv_a, pub_a, block_a.clone (), 0, sender, 0);
	});
}

namespace
{
class rollback_visitor : public rai::block_visitor
{
public:
    rollback_visitor (rai::ledger & ledger_a) :
    ledger (ledger_a)
    {
    }
    void send_block (rai::send_block const & block_a) override
    {
		auto hash (block_a.hash ());
        rai::receivable receivable;
		rai::transaction transaction (ledger.store.environment, nullptr, true);
		while (ledger.store.pending_get (transaction, hash, receivable))
		{
			ledger.rollback (transaction, ledger.latest (transaction, block_a.hashables.destination));
		}
        rai::account_info info;
        ledger.store.account_get (transaction, receivable.source, info);
		ledger.store.pending_del (transaction, hash);
        ledger.change_latest (transaction, receivable.source, block_a.hashables.previous, info.rep_block, ledger.balance (transaction, block_a.hashables.previous));
		ledger.store.block_del (transaction, hash);
    }
    void receive_block (rai::receive_block const & block_a) override
    {
		rai::transaction transaction (ledger.store.environment, nullptr, true);
		auto hash (block_a.hash ());
        auto representative (ledger.representative (transaction, block_a.hashables.source));
        auto amount (ledger.amount (transaction, block_a.hashables.source));
        auto destination_account (ledger.account (transaction, hash));
		ledger.move_representation (transaction, ledger.representative (transaction, hash), representative, amount);
        ledger.change_latest (transaction, destination_account, block_a.hashables.previous, representative, ledger.balance (transaction, block_a.hashables.previous));
		ledger.store.block_del (transaction, hash);
        ledger.store.pending_put (transaction, block_a.hashables.source, {ledger.account (transaction, block_a.hashables.source), amount, destination_account});
    }
    void open_block (rai::open_block const & block_a) override
    {
		rai::transaction transaction (ledger.store.environment, nullptr, true);
		auto hash (block_a.hash ());
        auto representative (ledger.representative (transaction, block_a.hashables.source));
        auto amount (ledger.amount (transaction, block_a.hashables.source));
        auto destination_account (ledger.account (transaction, hash));
		ledger.move_representation (transaction, ledger.representative (transaction, hash), representative, amount);
        ledger.change_latest (transaction, destination_account, 0, representative, 0);
		ledger.store.block_del (transaction, hash);
        ledger.store.pending_put (transaction, block_a.hashables.source, {ledger.account (transaction, block_a.hashables.source), amount, destination_account});
    }
    void change_block (rai::change_block const & block_a) override
    {
		rai::transaction transaction (ledger.store.environment, nullptr, true);
        auto representative (ledger.representative (transaction, block_a.hashables.previous));
        auto account (ledger.account (transaction, block_a.hashables.previous));
        rai::account_info info;
        ledger.store.account_get (transaction, account, info);
		ledger.move_representation (transaction, block_a.representative (), representative, ledger.balance (transaction, block_a.hashables.previous));
		ledger.store.block_del (transaction, block_a.hash ());
        ledger.change_latest (transaction, account, block_a.hashables.previous, representative, info.balance);
    }
    rai::ledger & ledger;
};
}

bool rai::parse_port (std::string const & string_a, uint16_t & port_a)
{
	bool result;
	size_t converted;
	port_a = std::stoul (string_a, &converted);
	result = converted != string_a.size () || converted > std::numeric_limits <uint16_t>::max ();
	return result;
}
bool rai::parse_address_port (std::string const & string, boost::asio::ip::address & address_a, uint16_t & port_a)
{
    auto result (false);
    auto port_position (string.rfind (':'));
    if (port_position != std::string::npos && port_position > 0)
    {
        std::string port_string (string.substr (port_position + 1));
        try
        {
			uint16_t port;
			result = parse_port (port_string, port);
            if (!result)
            {
                boost::system::error_code ec;
                auto address (boost::asio::ip::address_v6::from_string (string.substr (0, port_position), ec));
                if (ec == 0)
                {
                    address_a = address;
                    port_a = port;
                }
                else
                {
                    result = true;
                }
            }
            else
            {
                result = true;
            }
        }
        catch (...)
        {
            result = true;
        }
    }
    else
    {
        result = true;
    }
    return result;
}

bool rai::parse_endpoint (std::string const & string, rai::endpoint & endpoint_a)
{
    boost::asio::ip::address address;
    uint16_t port;
    auto result (parse_address_port (string, address, port));
    if (!result)
    {
        endpoint_a = rai::endpoint (address, port);
    }
    return result;
}

bool rai::parse_tcp_endpoint (std::string const & string, rai::tcp_endpoint & endpoint_a)
{
    boost::asio::ip::address address;
    uint16_t port;
    auto result (parse_address_port (string, address, port));
    if (!result)
    {
        endpoint_a = rai::tcp_endpoint (address, port);
    }
    return result;
}

void rai::node::start ()
{
    network.receive ();
    ongoing_keepalive ();
    bootstrap.start ();
	backup_wallet ();
	active.announce_votes ();
}

void rai::node::stop ()
{
    BOOST_LOG (log) << "Node stopping";
	active.roots.clear ();
    network.stop ();
    bootstrap.stop ();
}

void rai::node::keepalive_preconfigured (std::vector <std::string> const & peers_a)
{
	for (auto i (peers_a.begin ()), n (peers_a.end ()); i != n; ++i)
	{
		keepalive (*i, rai::network::node_port);
	}
}

rai::block_hash rai::node::latest (rai::account const & account_a)
{
	rai::transaction transaction (store.environment, nullptr, false);
	return ledger.latest (transaction, account_a);
}

rai::uint128_t rai::node::balance (rai::account const & account_a)
{
	rai::transaction transaction (store.environment, nullptr, false);
	return ledger.account_balance (transaction, account_a);
}

rai::uint128_t rai::node::weight (rai::account const & account_a)
{
	rai::transaction transaction (store.environment, nullptr, false);
	return ledger.weight (transaction, account_a);
}

rai::account rai::node::representative (rai::account const & account_a)
{
	rai::transaction transaction (store.environment, nullptr, false);
	rai::account_info info;
	rai::account result (0);
	if (!store.account_get (transaction, account_a, info))
	{
		result = info.rep_block;
	}
	return result;
}

void rai::node::ongoing_keepalive ()
{
    keepalive_preconfigured (config.preconfigured_peers);
    auto peers_l (peers.purge_list (std::chrono::system_clock::now () - cutoff));
    for (auto i (peers_l.begin ()), j (peers_l.end ()); i != j && std::chrono::system_clock::now () - i->last_attempt > period; ++i)
    {
        network.send_keepalive (i->endpoint);
    }
	auto node_l (shared_from_this ());
    alarm.add (std::chrono::system_clock::now () + period, [node_l] () { node_l->ongoing_keepalive ();});
}

void rai::node::backup_wallet ()
{
	rai::transaction transaction (store.environment, nullptr, false);
	for (auto i (wallets.items.begin ()), n (wallets.items.end ()); i != n; ++i)
	{
		auto backup_path (application_path / "backup");
		boost::filesystem::create_directories (backup_path);
		i->second->store.write_backup (transaction, backup_path / (i->first.to_string () + ".json"));
	}
	auto this_l (shared ());
	alarm.add (std::chrono::system_clock::now () + backup_interval, [this_l] ()
	{
		this_l->backup_wallet ();
	});
}

int rai::node::price (rai::uint128_t const & balance_a, int amount_a)
{
	assert (balance_a >= amount_a * rai::Grai_ratio);
	auto balance_l (balance_a);
	double result (0.0);
	for (auto i (0); i < amount_a; ++i)
	{
		balance_l -= rai::Grai_ratio;
		auto balance_scaled ((balance_l / rai::Mrai_ratio).convert_to <double> ());
		auto units (balance_scaled / 1000.0);
		auto unit_price (((free_cutoff - units) / free_cutoff) * price_max);
		result += std::min (std::max (0.0, unit_price), price_max);
	}
	return static_cast <int> (result * 100.0);
}

namespace {
class distributed_work : public std::enable_shared_from_this <distributed_work>
{
public:
distributed_work (std::shared_ptr <rai::node> const & node_a, rai::block_hash const & root_a) :
node (node_a),
root (root_a)
{
	completed.clear ();
	for (auto & i : node_a->config.work_peers)
	{
		outstanding.insert (boost::str (boost::format ("http://[%1%]:%2%") % i.first.to_string () % std::to_string (i.second)));
	}
}
void start ()
{
	if (!outstanding.empty ())
	{
		auto this_l (shared_from_this ());
		std::lock_guard <std::mutex> lock (mutex);
		for (auto const & i: outstanding)
		{
			node->background ([this_l, i] ()
			{
				boost::network::http::client client;
				boost::network::http::client::request request (i);
				std::string request_string;
				{
					boost::property_tree::ptree request;
					request.put ("action", "work_generate");
					request.put ("hash", this_l->root.to_string ());
					std::stringstream ostream;
					boost::property_tree::write_json (ostream, request);
					request_string = ostream.str ();
				}
				request.add_header (std::make_pair ("content-length", std::to_string (request_string.size ())));
				try
				{
					boost::network::http::client::response response = client.post (request, request_string, [this_l, i] (boost::iterator_range <char const *> const & range, boost::system::error_code const & ec)
					{
						this_l->callback (range, ec, i);
					});
					uint16_t status (boost::network::http::status (response));
					if (status != 200)
					{
						BOOST_LOG (this_l->node->log) << boost::str (boost::format ("Work peer %1% responded with an error %2%") % i % std::to_string (status));
						this_l->failure (i);
					}
				}
				catch (...)
				{
					BOOST_LOG (this_l->node->log) << boost::str (boost::format ("Unable to contact work peer %1%") % i);
					this_l->failure (i);
				}
			});
		}
	}
	else
	{
		handle_failure (true);
	}
}
void stop ()
{
	auto this_l (shared_from_this ());
	std::lock_guard <std::mutex> lock (mutex);
	for (auto const & i: outstanding)
	{
		node->background ([this_l, i] ()
		{
			boost::network::http::client client;
			boost::network::http::client::request request (i);
			std::string request_string;
			{
				boost::property_tree::ptree request;
				request.put ("action", "work_cancel");
				request.put ("hash", this_l->root.to_string ());
				std::stringstream ostream;
				boost::property_tree::write_json (ostream, request);
				request_string = ostream.str ();
			}
			request.add_header (std::make_pair ("content-length", std::to_string (request_string.size ())));
			try
			{
				boost::network::http::client::response response = client.post (request, request_string, [this_l, i] (boost::iterator_range <char const *> const & range, boost::system::error_code const & ec)
				{
				});
			}
			catch (...)
			{
			}
		});
	}
	outstanding.clear ();
}
void callback (boost::iterator_range <char const *> const & range, boost::system::error_code const & ec, std::string const & address)
{
	if (!ec)
	{
		success (range, address);
	}
	else
	{
		failure (address);
	}
}
void success (boost::iterator_range <char const *> const & range, std::string const & address)
{
	auto last (remove (address));
	std::string body;
	for (auto const & i: range)
	{
		body += i;
	}
	std::stringstream istream (body);
	try
	{
		boost::property_tree::ptree result;
		boost::property_tree::read_json (istream, result);
		auto work_text (result.get <std::string> ("work"));
		uint64_t work;
		if (!rai::from_string_hex (work_text, work))
		{
			if (node->work.work_validate (root, work))
			{
				set_once (work);
				stop ();
			}
			else
			{
				BOOST_LOG (node->log) << boost::str (boost::format ("Incorrect work response from %1% for root %2% value %3%") % address % root.to_string () % work_text);
				handle_failure (last);
			}
		}
		else
		{
			BOOST_LOG (node->log) << boost::str (boost::format ("Work response from %1% wasn't a number") % address % work_text);
			handle_failure (last);
		}
	}
	catch (...)
	{
		BOOST_LOG (node->log) << boost::str (boost::format ("Work response from %1% wasn't parsable") % address % body);
		handle_failure (last);
	}
}
void set_once (uint64_t work_a)
{
	if (!completed.test_and_set ())
	{
		promise.set_value (work_a);
	}
}
void failure (std::string const & address)
{
	auto last (remove (address));
	handle_failure (last);
}
void handle_failure (bool last)
{
	if (last)
	{
		if (!completed.test_and_set ())
		{
			promise.set_value (node->work.generate (root));
		}
	}
}
bool remove (std::string const & address)
{
	std::lock_guard <std::mutex> lock (mutex);
	outstanding.erase (address);
	return outstanding.empty ();
}
std::promise <uint64_t> promise;
std::shared_ptr <rai::node> node;
rai::block_hash root;
std::mutex mutex;
std::unordered_set <std::string> outstanding;
std::atomic_flag completed;
};
}

void rai::node::generate_work (rai::block & block_a)
{
    block_a.block_work_set (generate_work (block_a.root ()));
}

uint64_t rai::node::generate_work (rai::uint256_union const & hash_a)
{
	auto work_generation (std::make_shared <distributed_work> (shared (), hash_a));
	work_generation->start ();
	return work_generation->promise.get_future ().get ();
}

namespace
{
class confirmed_visitor : public rai::block_visitor
{
public:
    confirmed_visitor (rai::node & node_a) :
    node (node_a)
    {
    }
    void send_block (rai::send_block const & block_a) override
    {
        for (auto i (node.wallets.items.begin ()), n (node.wallets.items.end ()); i != n; ++i)
        {
			auto wallet (i->second);
			if (wallet->exists (block_a.hashables.destination))
			{
				rai::account representative;
				rai::receivable receivable;
				rai::transaction transaction (node.store.environment, nullptr, false);
				representative = wallet->store.representative (transaction);
				auto error (node.store.pending_get (transaction, block_a.hash (), receivable));
				if (!error)
				{
					auto block_l (std::shared_ptr <rai::send_block> (static_cast <rai::send_block *> (block_a.clone ().release ())));
					auto node_l (node.shared ());
					auto amount (receivable.amount.number ());
					wallet->receive_async (*block_l, representative, amount, [] (std::unique_ptr <rai::block> block_a) {});
				}
				else
				{
					if (node.config.logging.ledger_duplicate_logging ())
					{
						BOOST_LOG (node.log) << boost::str (boost::format ("Block confirmed before timeout %1%") % block_a.hash ().to_string ());
					}
				}
			}
        }
    }
    void receive_block (rai::receive_block const &) override
    {
    }
    void open_block (rai::open_block const &) override
    {
    }
    void change_block (rai::change_block const &) override
    {
    }
    rai::node & node;
};
}

void rai::node::process_confirmed (rai::block const & confirmed_a)
{
    confirmed_visitor visitor (*this);
    confirmed_a.visit (visitor);
}

void rai::node::process_message (rai::message & message_a, rai::endpoint const & sender_a)
{
	network_message_visitor visitor (*this, sender_a);
	message_a.visit (visitor);
}

rai::endpoint rai::network::endpoint ()
{
	boost::system::error_code ec;
	auto port (socket.local_endpoint (ec).port ());
	if (ec)
	{
		BOOST_LOG (node.log) << "Unable to retrieve port: " << ec.message ();
	}
    return rai::endpoint (boost::asio::ip::address_v6::loopback (), port);
}

void rai::peer_container::bootstrap_failed (rai::endpoint const & endpoint_a)
{
	std::lock_guard <std::mutex> lock (mutex);
	auto existing (peers.find (endpoint_a));
	if (existing != peers.end ())
	{
		peers.modify (existing, [] (rai::peer_information & info_a)
		{
			info_a.last_bootstrap_failure = std::chrono::system_clock::now ();
		});
	}
}

void rai::peer_container::random_fill (std::array <rai::endpoint, 8> & target_a)
{
    auto peers (list ());
    while (peers.size () > target_a.size ())
    {
        auto index (random_pool.GenerateWord32 (0, peers.size () - 1));
        assert (index < peers.size ());
		assert (index >= 0);
		if (index != peers.size () - 1)
		{
				peers [index] = peers [peers.size () - 1];
		}
        peers.pop_back ();
    }
    assert (peers.size () <= target_a.size ());
    auto endpoint (rai::endpoint (boost::asio::ip::address_v6 {}, 0));
    assert (endpoint.address ().is_v6 ());
    std::fill (target_a.begin (), target_a.end (), endpoint);
    auto j (target_a.begin ());
    for (auto i (peers.begin ()), n (peers.end ()); i != n; ++i, ++j)
    {
        assert (i->endpoint.address ().is_v6 ());
        assert (j < target_a.end ());
        *j = i->endpoint;
    }
}

std::vector <rai::peer_information> rai::peer_container::purge_list (std::chrono::system_clock::time_point const & cutoff)
{
	std::vector <rai::peer_information> result;
	{
		std::lock_guard <std::mutex> lock (mutex);
		auto pivot (peers.get <1> ().lower_bound (cutoff));
		result.assign (pivot, peers.get <1> ().end ());
		peers.get <1> ().erase (peers.get <1> ().begin (), pivot);
		for (auto i (peers.begin ()), n (peers.end ()); i != n; ++i)
		{
			peers.modify (i, [] (rai::peer_information & info) {info.last_attempt = std::chrono::system_clock::now ();});
		}
	}
	if (result.empty ())
	{
		disconnect_observer ();
	}
    return result;
}

size_t rai::peer_container::size ()
{
    std::lock_guard <std::mutex> lock (mutex);
    return peers.size ();
}

bool rai::peer_container::empty ()
{
    return size () == 0;
}

bool rai::peer_container::not_a_peer (rai::endpoint const & endpoint_a)
{
    bool result (false);
    if (endpoint_a.address ().to_v6 ().is_unspecified ())
    {
        result = true;
    }
    else if (rai::reserved_address (endpoint_a))
    {
        result = true;
    }
    else if (endpoint_a == self)
    {
        result = true;
    }
    return result;
}

bool rai::peer_container::insert (rai::endpoint const & endpoint_a)
{
    return insert (endpoint_a, rai::block_hash (0));
}

bool rai::peer_container::knows_about (rai::endpoint const & endpoint_a, rai::block_hash const & hash_a)
{
    std::lock_guard <std::mutex> lock (mutex);
    bool result (false);
    auto existing (peers.find (endpoint_a));
    if (existing != peers.end ())
    {
        result = existing->most_recent == hash_a;
    }
    return result;
}

bool rai::peer_container::insert (rai::endpoint const & endpoint_a, rai::block_hash const & hash_a)
{
	auto unknown (false);
    auto result (not_a_peer (endpoint_a));
    if (!result)
    {
        std::lock_guard <std::mutex> lock (mutex);
        auto existing (peers.find (endpoint_a));
        if (existing != peers.end ())
        {
            peers.modify (existing, [&hash_a] (rai::peer_information & info)
            {
                info.last_contact = std::chrono::system_clock::now ();
                info.most_recent = hash_a;
            });
            result = true;
        }
        else
        {
            peers.insert ({endpoint_a, std::chrono::system_clock::now (), std::chrono::system_clock::now (), std::chrono::system_clock::time_point (), hash_a});
			unknown = true;
        }
    }
	if (unknown)
	{
		peer_observer (endpoint_a);
	}
    return result;
}

namespace {
boost::asio::ip::address_v6 mapped_from_v4_bytes (unsigned long address_a)
{
    return boost::asio::ip::address_v6::v4_mapped (boost::asio::ip::address_v4 (address_a));
}
}

bool rai::reserved_address (rai::endpoint const & endpoint_a)
{
    assert (endpoint_a.address ().is_v6 ());
	auto bytes (endpoint_a.address ().to_v6 ());
	auto result (false);
    if (bytes >= mapped_from_v4_bytes (0x00000000ul) && bytes <= mapped_from_v4_bytes (0x00fffffful)) // Broadcast RFC1700
	{
		result = true;
	}
	else if (bytes >= mapped_from_v4_bytes (0xc0000200ul) && bytes <= mapped_from_v4_bytes (0xc00002fful)) // TEST-NET RFC5737
	{
		result = true;
	}
	else if (bytes >= mapped_from_v4_bytes (0xc6336400ul) && bytes <= mapped_from_v4_bytes (0xc63364fful)) // TEST-NET-2 RFC5737
	{
		result = true;
	}
	else if (bytes >= mapped_from_v4_bytes (0xcb007100ul) && bytes <= mapped_from_v4_bytes (0xcb0071fful)) // TEST-NET-3 RFC5737
	{
		result = true;
	}
	else if (bytes >= mapped_from_v4_bytes (0xe9fc0000ul) && bytes <= mapped_from_v4_bytes (0xe9fc00fful))
	{
		result = true;
	}
	else if (bytes >= mapped_from_v4_bytes (0xf0000000ul)) // Reserved RFC6890
	{
		result = true;
	}
	return result;
}

rai::peer_container::peer_container (rai::endpoint const & self_a) :
self (self_a),
peer_observer ([] (rai::endpoint const &) {}),
disconnect_observer ([] () {})
{
}

void rai::peer_container::contacted (rai::endpoint const & endpoint_a)
{
    auto endpoint_l (endpoint_a);
    if (endpoint_l.address ().is_v4 ())
    {
        endpoint_l = rai::endpoint (boost::asio::ip::address_v6::v4_mapped (endpoint_l.address ().to_v4 ()), endpoint_l.port ());
    }
    assert (endpoint_l.address ().is_v6 ());
	insert (endpoint_l);
}

std::ostream & operator << (std::ostream & stream_a, std::chrono::system_clock::time_point const & time_a)
{
    time_t last_contact (std::chrono::system_clock::to_time_t (time_a));
    std::string string (ctime (&last_contact));
    string.pop_back ();
    stream_a << string;
    return stream_a;
}

void rai::network::initiate_send ()
{
	assert (!socket_mutex.try_lock ());
	assert (!sends.empty ());
	auto & front (sends.front ());
	if (node.config.logging.network_packet_logging ())
	{
		BOOST_LOG (node.log) << "Sending packet";
	}
	socket.async_send_to (boost::asio::buffer (front.data, front.size), front.endpoint, [this, front] (boost::system::error_code const & ec, size_t size_a)
	{
		if (front.rebroadcast > 0)
		{
			this->node.alarm.add (std::chrono::system_clock::now () + std::chrono::seconds (this->node.config.rebroadcast_delay), [this, front]
			{
				send_buffer (front.data, front.size, front.endpoint, front.rebroadcast - 1, front.callback);
			});
		}
		else
		{
			rai::send_info self;
			{
				std::unique_lock <std::mutex> lock (socket_mutex);
				assert (!sends.empty ());
				self = sends.front ();
			}
			self.callback (ec, size_a);
		}
		send_complete (ec, size_a);
	});
}

void rai::network::send_buffer (uint8_t const * data_a, size_t size_a, rai::endpoint const & endpoint_a, size_t rebroadcast_a, std::function <void (boost::system::error_code const &, size_t)> callback_a)
{
	std::unique_lock <std::mutex> lock (socket_mutex);
	auto initiate (sends.empty ());
	sends.push ({data_a, size_a, endpoint_a, rebroadcast_a, callback_a});
	if (initiate)
	{
		initiate_send ();
	}
}

void rai::network::send_complete (boost::system::error_code const & ec, size_t size_a)
{
    if (node.config.logging.network_packet_logging ())
    {
        BOOST_LOG (node.log) << "Packet send complete";
    }
	std::unique_lock <std::mutex> lock (socket_mutex);
	assert (!sends.empty ());
	sends.pop ();
	if (!sends.empty ())
	{
		if (node.config.logging.network_packet_logging ())
		{
			BOOST_LOG (node.log) << boost::str (boost::format ("Delaying next packet send %1% microseconds") % node.config.packet_delay_microseconds);
		}
		node.alarm.add (std::chrono::system_clock::now () + std::chrono::microseconds (node.config.packet_delay_microseconds), [this] ()
		{
			std::unique_lock <std::mutex> lock (socket_mutex);
			initiate_send ();
		});
	}
}

uint64_t rai::block_store::now ()
{
    boost::posix_time::ptime epoch (boost::gregorian::date (1970, 1, 1));
    auto now (boost::posix_time::second_clock::universal_time ());
    auto diff (now - epoch);
    return diff.total_seconds ();
}

bool rai::peer_container::known_peer (rai::endpoint const & endpoint_a)
{
    std::lock_guard <std::mutex> lock (mutex);
    auto existing (peers.find (endpoint_a));
    return existing != peers.end () && existing->last_contact > std::chrono::system_clock::now () - rai::node::cutoff;
}

std::shared_ptr <rai::node> rai::node::shared ()
{
    return shared_from_this ();
}

rai::election::election (rai::node & node_a, rai::block const & block_a, std::function <void (rai::block &)> const & confirmation_action_a) :
confirmation_action (confirmation_action_a),
votes (block_a),
node (node_a),
last_vote (std::chrono::system_clock::now ()),
last_winner (block_a.clone ())
{
	confirmed.clear ();
}

void rai::election::recompute_winner ()
{
	auto last_winner_l (last_winner);
	for (auto i (node.wallets.items.begin ()), n (node.wallets.items.end ()); i != n; ++i)
	{
		auto is_representative (false);
		rai::vote vote_l;
		{
			rai::transaction transaction (node.store.environment, nullptr, false);
			is_representative = i->second->store.is_representative (transaction);
			if (is_representative)
			{
				auto representative (i->second->store.representative (transaction));
				rai::raw_key prv;
				is_representative = !i->second->store.fetch (transaction, representative, prv);
				if (is_representative)
				{
					vote_l = rai::vote (representative, prv, 0, last_winner_l->clone ());
				}
				else
				{
					BOOST_LOG (node.log) << boost::str (boost::format ("Unable to vote on block due to locked wallet %1%") % i->first.to_string ());
				}
			}
		}
		if (is_representative)
		{
			vote (vote_l);
		}
	}
}

void rai::election::broadcast_winner ()
{
	recompute_winner ();
	std::unique_ptr <rai::block> winner_l;
	{
		rai::transaction transaction (node.store.environment, nullptr, false);
		winner_l = node.ledger.winner (transaction, votes).second;
	}
	assert (winner_l != nullptr);
	auto list (node.peers.list ());
	node.network.confirm_broadcast (list, std::move (winner_l), votes.sequence, 0);
}

rai::uint128_t rai::election::quorum_threshold (MDB_txn * transaction_a, rai::ledger & ledger_a)
{
    return ledger_a.supply (transaction_a) / 2;
}

void rai::election::confirm_once ()
{
	auto confirmed_l (confirmed.test_and_set ());
	if (!confirmed_l)
	{
		auto winner_l (last_winner);
		auto confirmation_action_l (confirmation_action);
		node.background ([winner_l, confirmation_action_l] ()
		{
			confirmation_action_l (*winner_l);
		});
	}
}

bool rai::election::recalculate_winner ()
{
	auto result (false);
	rai::transaction transaction (node.store.environment, nullptr, true);
	auto tally_l (node.ledger.tally (transaction, votes));
	assert (tally_l.size () > 0);
	auto quorum_threshold_l (quorum_threshold (transaction, node.ledger));
	auto winner (std::move (tally_l.begin ()));
	if (!(*winner->second == *last_winner) && (winner->first > quorum_threshold_l))
	{
		// Replace our block with the winner and roll back any dependent blocks
		node.ledger.rollback (transaction, last_winner->hash ());
		node.ledger.process (transaction, *winner->second);
		last_winner = std::move (winner->second);
	}
	// Check if we can do a fast confirm for the usual case of good actors
	if (tally_l.size () == 1)
	{
		// No forks detected
		if (tally_l.begin ()->first > quorum_threshold (transaction, node.ledger))
		{
			// We have vote quarum
			result = true;
		}
	}
	return result;
}

void rai::election::confirm_if_quarum ()
{
	auto quarum (recalculate_winner ());
	if (quarum)
	{
		confirm_once ();
	}
}

void rai::election::confirm_cutoff ()
{
	confirm_once ();
}

void rai::election::vote (rai::vote const & vote_a)
{
	auto tally_changed (votes.vote (vote_a));
	if (tally_changed)
	{
		confirm_if_quarum ();
	}
}

void rai::active_transactions::announce_votes ()
{
	std::vector <rai::block_hash> inactive;
	std::lock_guard <std::mutex> lock (mutex);
	size_t announcements (0);
	{
		auto i (roots.begin ());
		auto n (roots.end ());
		// Announce our decision for up to `announcements_per_interval' conflicts
		for (; i != n && announcements < announcements_per_interval; ++i)
		{
			auto election_l (i->election);
			node.background ([election_l] () { election_l->broadcast_winner (); } );
			if (i->announcements >= contigious_announcements - 1)
			{
				// These blocks have reached the confirmation interval for forks
				i->election->confirm_cutoff ();
				auto root_l (i->election->votes.id);
				inactive.push_back (root_l);
			}
			else
			{
				roots.modify (i, [] (rai::conflict_info & info_a)
				{
					++info_a.announcements;
				});
			}
		}
		// Mark remainder as 0 announcements sent
		// This could happen if there's a flood of forks, the network will resolve them in increasing root hash order
		// This is a DoS protection mechanism to rate-limit the amount of traffic for solving forks.
		for (; i != n; ++i)
		{
			// Reset announcement count for conflicts above announcement cutoff
			roots.modify (i, [] (rai::conflict_info & info_a)
			{
				info_a.announcements = 0;
			});
		}
	}
	for (auto i (inactive.begin ()), n (inactive.end ()); i != n; ++i)
	{
		assert (roots.find (*i) != roots.end ());
		roots.erase (*i);
	}
	auto now (std::chrono::system_clock::now ());
	auto node_l (node.shared ());
	node.alarm.add ((rai::rai_network == rai::rai_networks::rai_test_network) ? now + std::chrono::milliseconds (10) : now + std::chrono::seconds (16), [node_l] () {node_l->active.announce_votes ();});
}

void rai::active_transactions::start (rai::block const & block_a, std::function <void (rai::block &)> const & confirmation_action_a)
{
    std::lock_guard <std::mutex> lock (mutex);
    auto root (block_a.root ());
    auto existing (roots.find (root));
    if (existing == roots.end ())
    {
        auto election (std::make_shared <rai::election> (node, block_a, confirmation_action_a));
        roots.insert (rai::conflict_info {root, election, 0});
    }
}

// Validate a vote and apply it to the current election if one exists
void rai::active_transactions::vote (rai::vote const & vote_a)
{
	std::shared_ptr <rai::election> election;
	{
		std::lock_guard <std::mutex> lock (mutex);
		auto root (vote_a.block->root ());
		auto existing (roots.find (root));
		if (existing != roots.end ())
		{
			election = existing->election;
		}
	}
	if (election)
	{
        election->vote (vote_a);
	}
}

rai::active_transactions::active_transactions (rai::node & node_a) :
node (node_a)
{
}

int rai::node::store_version ()
{
	rai::transaction transaction (store.environment, nullptr, false);
	return store.version_get (transaction);
}

rai::fan::fan (rai::uint256_union const & key, size_t count_a)
{
    std::unique_ptr <rai::uint256_union> first (new rai::uint256_union (key));
    for (auto i (1); i < count_a; ++i)
    {
        std::unique_ptr <rai::uint256_union> entry (new rai::uint256_union);
        random_pool.GenerateBlock (entry->bytes.data (), entry->bytes.size ());
        *first ^= *entry;
        values.push_back (std::move (entry));
    }
    values.push_back (std::move (first));
}

void rai::fan::value (rai::raw_key & prv_a)
{
    prv_a.data.clear ();
    for (auto & i: values)
    {
        prv_a.data ^= *i;
    }
}

void rai::fan::value_set (rai::raw_key const & value_a)
{
    rai::raw_key value_l;
	value (value_l);
    *(values [0]) ^= value_l.data;
    *(values [0]) ^= value_a.data;
}

rai::thread_runner::thread_runner (boost::asio::io_service & service_a, unsigned service_threads_a)
{
	for (auto i (0); i < service_threads_a; ++i)
	{
		threads.push_back (std::thread ([&service_a] ()
		{
			try
			{
				service_a.run ();
			}
			catch (...)
			{
				assert (false && "Unhandled service exception");
			}
		}));
	}
}

void rai::thread_runner::join ()
{
	for (auto &i : threads)
	{
		i.join ();
	}
}

void rai::add_node_options (boost::program_options::options_description & description_a)
{
	description_a.add_options ()
	("account_get", "Get account number for the <key>")
	("account_key", "Get the public key for <account>")
	("diagnostics", "Run internal diagnostics")
	("key_create", "Generates a random keypair")
	("key_expand", "Derive public key and account number from <key>")
	("wallet_add_adhoc", "Insert <key> in to <wallet>")
	("wallet_add_next", "Insert next deterministic key in to <wallet>")
	("wallet_create", "Creates a new wallet and prints the ID")
	("wallet_decrypt_unsafe", "Decrypts <wallet> using <password>, !!THIS WILL PRINT YOUR PRIVATE KEY TO STDOUT!!")
	("wallet_destroy", "Destroys <wallet> and all keys it contains")
	("wallet_import", "Imports keys in <file> using <password> in to <wallet>")
	("wallet_list", "Dumps wallet IDs and public keys")
	("wallet_remove", "Remove <account> from <wallet>")
	("wallet_representative_get", "Prints default representative for <wallet>")
	("wallet_representative_set", "Set <account> as default representative for <wallet>")
	("account", boost::program_options::value <std::string> (), "Defines <account> for other commands")
	("file", boost::program_options::value <std::string> (), "Defines <file> for other commands")
	("key", boost::program_options::value <std::string> (), "Defines the <key> for other commands, hex")
	("password", boost::program_options::value <std::string> (), "Defines <password> for other commands")
	("wallet", boost::program_options::value <std::string> (), "Defines <wallet> for other commands");
}

bool rai::handle_node_options (boost::program_options::variables_map & vm)
{
	auto result (false);
    if (vm.count ("account_get") > 0)
    {
		if (vm.count ("key") == 1)
		{
			rai::uint256_union pub;
			pub.decode_hex (vm ["key"].as <std::string> ());
			std::cout << "Account: " << pub.to_account () << std::endl;
		}
		else
		{
			std::cerr << "account comand requires one <key> option";
			result = true;
		}
    }
	else if (vm.count ("account_key") > 0)
	{
		if (vm.count ("account") == 1)
		{
			rai::uint256_union account;
			account.decode_account (vm ["account"].as <std::string> ());
			std::cout << "Hex: " << account.to_string () << std::endl;
		}
		else
		{
			std::cerr << "account_key command requires one <account> option";
			result = true;
		}
	}
	else if (vm.count ("diagnostics"))
	{
		std::cout << "Testing hash function" << std::endl;
		rai::raw_key key;
		key.data.clear ();
		rai::send_block send (0, 0, 0, key, 0, 0);
		auto hash (send.hash ());
		std::cout << "Testing key derivation function" << std::endl;
		rai::raw_key junk1;
		junk1.data.clear ();
		rai::uint256_union junk2 (0);
		rai::kdf kdf;
		kdf.phs (junk1, "", junk2);
	}
    else if (vm.count ("key_create"))
    {
        rai::keypair pair;
        std::cout << "Private: " << pair.prv.data.to_string () << std::endl << "Public: " << pair.pub.to_string () << std::endl << "Account: " << pair.pub.to_account () << std::endl;
    }
	else if (vm.count ("key_expand"))
	{
		if (vm.count ("key") == 1)
		{
			rai::uint256_union prv;
			prv.decode_hex (vm ["key"].as <std::string> ());
			rai::uint256_union pub;
			ed25519_publickey (prv.bytes.data (), pub.bytes.data ());
			std::cout << "Private: " << prv.to_string () << std::endl << "Public: " << pub.to_string () << std::endl << "Account: " << pub.to_account () << std::endl;
		}
		else
		{
			std::cerr << "key_expand command requires one <key> option";
			result = true;
		}
	}
	else if (vm.count ("wallet_add_adhoc"))
	{
		if (vm.count ("wallet") == 1 && vm.count ("key") == 1)
		{
			rai::uint256_union wallet_id;
			if (!wallet_id.decode_hex (vm ["wallet"].as <std::string> ()))
			{
				std::string password;
				if (vm.count ("password") > 0)
				{
					password = vm ["password"].as <std::string> ();
				}
				inactive_node node;
				auto wallet (node.node->wallets.open (wallet_id));
				if (wallet != nullptr)
				{
					if (!wallet->enter_password (password))
					{
						rai::transaction transaction (wallet->store.environment, nullptr, true);
						rai::raw_key key;
						key.data.decode_hex (vm ["key"].as <std::string> ());
						wallet->store.insert_adhoc (transaction, key);
					}
					else
					{
						std::cerr << "Invalid password\n";
						result = true;
					}
				}
				else
				{
					std::cerr << "Wallet doesn't exist\n";
					result = true;
				}
			}
			else
			{
				std::cerr << "Invalid wallet id\n";
				result = true;
			}
		}
		else
		{
			std::cerr << "wallet_add command requires one <wallet> option and one <key> option and optionally one <password> option";
			result = true;
		}
	}
	else if (vm.count ("wallet_add_next"))
	{
		if (vm.count ("wallet") == 1)
		{
			rai::uint256_union wallet_id;
			if (!wallet_id.decode_hex (vm ["wallet"].as <std::string> ()))
			{
				std::string password;
				if (vm.count ("password") > 0)
				{
					password = vm ["password"].as <std::string> ();
				}
				inactive_node node;
				auto wallet (node.node->wallets.open (wallet_id));
				if (wallet != nullptr)
				{
					if (!wallet->enter_password (password))
					{
						rai::transaction transaction (wallet->store.environment, nullptr, true);
						auto pub (wallet->store.deterministic_insert (transaction));
						std::cout << boost::str (boost::format ("Account: %1%\n") % pub.to_account ());
					}
					else
					{
						std::cerr << "Invalid password\n";
						result = true;
					}
				}
				else
				{
					std::cerr << "Wallet doesn't exist\n";
					result = true;
				}
			}
			else
			{
				std::cerr << "Invalid wallet id\n";
				result = true;
			}
		}
		else
		{
			std::cerr << "wallet_add command requires one <wallet> option and one <key> option and optionally one <password> option";
			result = true;
		}
	}
	else if (vm.count ("wallet_create"))
	{
		inactive_node node;
		rai::keypair key;
		std::cout << key.pub.to_string () << std::endl;
		auto wallet (node.node->wallets.create (key.pub));
		wallet->enter_initial_password ();
	}
	else if (vm.count ("wallet_decrypt_unsafe"))
	{
		if (vm.count ("wallet") == 1)
		{
			std::string password;
			if (vm.count ("password") == 1)
			{
				password = vm ["password"].as <std::string> ();
			}
			rai::uint256_union wallet_id;
			if (!wallet_id.decode_hex (vm ["wallet"].as <std::string> ()))
			{
				inactive_node node;
				auto existing (node.node->wallets.items.find (wallet_id));
				if (existing != node.node->wallets.items.end ())
				{
					if (!existing->second->enter_password (password))
					{
						rai::transaction transaction (existing->second->store.environment, nullptr, false);
						rai::raw_key seed;
						existing->second->store.seed (seed, transaction);
						std::cerr << boost::str (boost::format ("Seed: %1%\n") % seed.data.to_string ());
						for (auto i (existing->second->store.begin (transaction)), m (existing->second->store.end ()); i != m; ++i)
						{
							rai::account account (i->first);
							rai::raw_key key;
							auto error (existing->second->store.fetch (transaction, account, key));
							assert (!error);
							std::cerr << boost::str (boost::format ("Pub: %1% Prv: %2%\n") % account.to_account () % key.data.to_string ());
						}
					}
					else
					{
						std::cerr << "Invalid password\n";
						result = true;
					}
				}
				else
				{
					std::cerr << "Wallet doesn't exist\n";
					result = true;
				}
			}
			else
			{
				std::cerr << "Invalid wallet id\n";
				result = true;
			}
		}
		else
		{
			std::cerr << "wallet_decrypt_unsafe requires one <wallet> option";
			result = true;
		}
	}
	else if (vm.count ("wallet_destroy"))
	{
		if (vm.count ("wallet") == 1)
		{
			rai::uint256_union wallet_id;
			if (!wallet_id.decode_hex (vm ["wallet"].as <std::string> ()))
			{
				inactive_node node;
				if (node.node->wallets.items.find (wallet_id) != node.node->wallets.items.end ())
				{
					node.node->wallets.destroy (wallet_id);
				}
				else
				{
					std::cerr << "Wallet doesn't exist\n";
					result = true;
				}
			}
			else
			{
				std::cerr << "Invalid wallet id\n";
				result = true;
			}
		}
		else
		{
			std::cerr << "wallet_destroy requires one <wallet> option";
			result = true;
		}
	}
	else if (vm.count ("wallet_import"))
	{
		if (vm.count ("file") == 1)
		{
			std::string filename (vm ["file"].as <std::string> ());
			std::ifstream stream;
			stream.open (filename.c_str ());
			if (!stream.fail ())
			{
				std::stringstream contents;
				contents << stream.rdbuf ();
				std::string password;
				if (vm.count ("password") == 1)
				{
					password = vm ["password"].as <std::string> ();
				}
				if (vm.count ("wallet") == 1)
				{
					rai::uint256_union wallet_id;
					if (!wallet_id.decode_hex (vm ["wallet"].as <std::string> ()))
					{
						inactive_node node;
						auto existing (node.node->wallets.items.find (wallet_id));
						if (existing != node.node->wallets.items.end ())
						{
							if (!existing->second->import (contents.str (), password))
							{
								result = false;
							}
							else
							{
								std::cerr << "Unable to import wallet\n";
								result = true;
							}
						}
						else
						{
							std::cerr << "Wallet doesn't exist\n";
							result = true;
						}
					}
					else
					{
						std::cerr << "Invalid wallet id\n";
						result = true;
					}
				}
				else
				{
					std::cerr << "wallet_destroy requires one <wallet> option\n";
					result = true;
				}
			}
			else
			{
				std::cerr << "Unable to open <file>\n";
				result = true;
			}
		}
		else
		{
			std::cerr << "wallet_import requires one <file> option\n";
			result = true;
		}
	}
	else if (vm.count ("wallet_list"))
	{
		inactive_node node;
		for (auto i (node.node->wallets.items.begin ()), n (node.node->wallets.items.end ()); i != n; ++i)
		{
			std::cout << boost::str (boost::format ("Wallet ID: %1%\n") % i->first.to_string ());
			rai::transaction transaction (i->second->store.environment, nullptr, false);
			for (auto j (i->second->store.begin (transaction)), m (i->second->store.end ()); j != m; ++j)
			{
				std::cout << rai::uint256_union (j->first).to_account () << '\n';
			}
		}
	}
	else if (vm.count ("wallet_remove"))
	{
		if (vm.count ("wallet") == 1 && vm.count ("account") == 1)
		{
			inactive_node node;
			rai::uint256_union wallet_id;
			if (!wallet_id.decode_hex (vm ["wallet"].as <std::string> ()))
			{
				auto wallet (node.node->wallets.items.find (wallet_id));
				if (wallet != node.node->wallets.items.end ())
				{
					rai::account account_id;
					if (!account_id.decode_account (vm ["account"].as <std::string> ()))
					{
						rai::transaction transaction (wallet->second->store.environment, nullptr, true);
						auto account (wallet->second->store.find (transaction, account_id));
						if (account != wallet->second->store.end ())
						{
							wallet->second->store.erase (transaction, account_id);
						}
						else
						{
							std::cerr << "Account not found in wallet\n";
							result = true;
						}
					}
					else
					{
						std::cerr << "Invalid account id\n";
						result = true;
					}
				}
				else
				{
					std::cerr << "Wallet not found\n";
					result = true;
				}
			}
			else
			{
				std::cerr << "Invalid wallet id\n";
				result = true;
			}
		}
		else
		{
			std::cerr << "wallet_remove command requires one <wallet> and one <account> option\n";
			result = true;
		}
	}
	else if (vm.count ("wallet_representative_get"))
	{
		if (vm.count ("wallet") == 1)
		{
			rai::uint256_union wallet_id;
			if (!wallet_id.decode_hex (vm ["wallet"].as <std::string> ()))
			{
				inactive_node node;
				auto wallet (node.node->wallets.items.find (wallet_id));
				if (wallet != node.node->wallets.items.end ())
				{
					rai::transaction transaction (wallet->second->store.environment, nullptr, false);
					auto representative (wallet->second->store.representative (transaction));
					std::cout << boost::str (boost::format ("Representative: %1%\n") % representative.to_account ());
				}
				else
				{
					std::cerr << "Wallet not found\n";
					result = true;
				}
			}
			else
			{
				std::cerr << "Invalid wallet id\n";
				result = true;
			}
		}
		else
		{
			std::cerr << "wallet_representative_get requires one <wallet> option\n";
			result = true;
		}
	}
	else if (vm.count ("wallet_representative_set"))
	{
		if (vm.count ("wallet") == 1)
		{
			if (vm.count ("account") == 1)
			{
				rai::uint256_union wallet_id;
				if (!wallet_id.decode_hex (vm ["wallet"].as <std::string> ()))
				{
					rai::account account;
					if (!account.decode_account (vm ["account"].as <std::string> ()))
					{
						inactive_node node;
						auto wallet (node.node->wallets.items.find (wallet_id));
						if (wallet != node.node->wallets.items.end ())
						{
							rai::transaction transaction (wallet->second->store.environment, nullptr, true);
							wallet->second->store.representative_set (transaction, account);
						}
						else
						{
							std::cerr << "Wallet not found\n";
							result = true;
						}
					}
					else
					{
						std::cerr << "Invalid account\n";
						result = true;
					}
				}
				else
				{
					std::cerr << "Invalid wallet id\n";
					result = true;
				}
			}
			else
			{
				std::cerr << "wallet_representative_set requires one <account> option\n";
				result = true;
			}
		}
		else
		{
			std::cerr << "wallet_representative_set requires one <wallet> option\n";
			result = true;
		}
	}
	else
	{
		result = true;
	}
	return result;
}

rai::inactive_node::inactive_node () :
service (boost::make_shared <boost::asio::io_service> ()),
alarm (*service)
{
	auto working (rai::working_path ());
	boost::filesystem::create_directories (working);
	node = std::make_shared <rai::node> (init, *service, 24000,  working, alarm, logging, work);
}