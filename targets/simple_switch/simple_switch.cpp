#include <iostream>
#include <memory>
#include <thread>
#include <fstream>

#include "behavioral_sim/queue.h"
#include "behavioral_sim/packet.h"
#include "behavioral_sim/parser.h"
#include "behavioral_sim/P4Objects.h"
#include "behavioral_sim/tables.h"
#include "behavioral_sim/switch.h"

#include "simple_switch.h"
#include "simple_switch_primitives.h"

class SimpleSwitch : public Switch {
public:
  SimpleSwitch(transmit_fn_t transmit_fn)
    : input_buffer(1024), transmit_fn(transmit_fn) {}

  int receive(int port_num, const char *buffer, int len) {
    static int pkt_id = 0;
    input_buffer.push_front(
        std::unique_ptr<Packet>(
	    new Packet(port_num, pkt_id++, 0, PacketBuffer(2048, buffer, len))
        )
    );
    return 0;
  }

  void start_and_return() {
    std::thread t(&SimpleSwitch::pipeline_thread, this);
    t.detach();    
  }

private:
  void pipeline_thread();

  int transmit(const Packet &packet) {
    std::cout<< "transmitting packet " << packet.get_packet_id() << std::endl;
    transmit_fn(packet.get_egress_port(), packet.data(), packet.get_data_size());
    return 0;
  }

private:
  Queue<unique_ptr<Packet> > input_buffer;
  transmit_fn_t transmit_fn;
};


void SimpleSwitch::pipeline_thread() {
  Pipeline *ingress_mau = p4objects->get_pipeline("ingress");
  Parser *parser = p4objects->get_parser("parser");
  Deparser *deparser = p4objects->get_deparser("deparser");
  PHV &phv = p4objects->get_phv();

  while(1) {
    unique_ptr<Packet> packet;
    input_buffer.pop_back(&packet);
    std::cout<< "processing packet " << packet->get_packet_id() << std::endl;
    
    parser->parse(packet.get(), &phv);
    ingress_mau->apply(*packet.get(), &phv);
    deparser->deparse(phv, packet.get());

    int egress_port = phv.get_field("standard_metadata.egress_spec").get_int();
    std::cout << "egress port is " << egress_port << std::endl;

    if(egress_port == 0) {
      std::cout << "dropping packet\n";
    }
    else {
      packet->set_egress_port(egress_port);
      transmit(*packet);
    }
  }
}

/* Switch instance */

static SimpleSwitch *simple_switch;


/* For test purposes only, temporary */

static void add_test_entry(void) {
  P4Objects *p4objects = simple_switch->get_p4objects();
  ExactMatchTable *table = p4objects->get_exact_match_table("forward");
  assert(table);

  entry_handle_t hdl;
  ActionFn *action_fn = p4objects->get_action("set_egress_port");
  ActionFnEntry action_entry(action_fn);
  action_entry.push_back_action_data(2);
  const MatchTable *next_table = nullptr;
  ByteContainer key("0xaabbccddeeff");

  table->add_entry(ExactMatchEntry(key, action_entry, next_table), &hdl);

  /* default behavior */

  ActionFn *default_action_fn = p4objects->get_action("_drop");
  ActionFnEntry default_action_entry(default_action_fn);
  const MatchTable *default_next_table = nullptr;

  table->set_default_action(default_action_entry, default_next_table);
}


/* C bindings */

extern "C" {

int packet_accept(int port_num, const char *buffer, int len) {
  return simple_switch->receive(port_num, buffer, len);
}

void start_processing(transmit_fn_t transmit_fn) {
  simple_switch = new SimpleSwitch(transmit_fn);
  simple_switch->init_objects("simple_switch.json");

  add_test_entry();

  simple_switch->start_and_return();
}

}
