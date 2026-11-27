#!/usr/bin/env python3
"""Which candidate pairs the source-built peer's ICE agent makes.

The pairing functions and their types are taken from the source text and
compiled for this machine with an agent that has only the fields they use.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tools.tests.test_esp_peer_source_transport import function

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / 'third_party/esp_peer/src/core'


def typedef(text: str, name: str) -> str:
    found = re.search(r'typedef\s+(?:enum|struct)\s*\{[^}]*(?:\{[^}]*\}[^}]*)*\}\s*' + re.escape(name) + r'\s*;', text)
    if found is None:
        raise AssertionError(f'missing source type {name}')
    return found.group(0)


HARNESS = r'''
static bool force_tcp_only;
typedef struct {
    uint8_t max_paired_candidates, candidate_pairs_num, local_candidates_count, remote_candidates_count;
    ice_candidate_t *local_candidates, *remote_candidates;
    ice_candidate_pair_t *candidate_pairs, **sort_pairs;
} agent_t;
'''

TESTS = r'''
enum { IPV4 = 2, IPV6 = 10 };
static ice_candidate_t candidate(ice_candidate_type_t type, ice_candidate_transport_t transport,
                                 uint8_t family, uint8_t last, uint32_t priority)
{
    ice_candidate_t c = {.component = 1, .priority = priority, .transport = transport, .type = type};
    c.addr.family = family; c.addr.port = 5000; c.addr.ipv4[3] = last;
    return c;
}
#define UDP ICE_CANDIDATE_TRANSPORT_UDP
#define TCP ICE_CANDIDATE_TRANSPORT_TCP
#define HOST ICE_CANDIDATE_TYPE_HOST
#define SRFLX ICE_CANDIDATE_TYPE_SRFLX
#define RELAY ICE_CANDIDATE_TYPE_RELAY

static ice_candidate_pair_t pairs[20], *sorted[20];
static agent_t agent_of(ice_candidate_t *local, int locals, ice_candidate_t *remote, int remotes)
{
    memset(pairs, 0, sizeof(pairs));
    agent_t agent = {.max_paired_candidates = 20, .local_candidates_count = locals, .remote_candidates_count = remotes,
                     .local_candidates = local, .remote_candidates = remote, .candidate_pairs = pairs, .sort_pairs = sorted};
    jetkvm_ice_pair_candidates(&agent);
    return agent;
}
/* The type of the pair of these two candidates, or NONE when there is none. */
static ice_candidate_type_t pair_type(agent_t *agent, ice_candidate_t *local, ice_candidate_t *remote)
{
    for (int i = 0; i < agent->candidate_pairs_num; ++i)
        if (pairs[i].local == local && pairs[i].remote == remote) return pairs[i].type;
    return ICE_CANDIDATE_TYPE_NONE;
}

int main(void)
{
    /* The cloud path: the Mini has a host and a server-reflexive candidate
     * on one socket, the browser has relay candidates only. */
    ice_candidate_t mini[] = {candidate(HOST, UDP, IPV4, 1, 2000), candidate(SRFLX, UDP, IPV4, 2, 1500)};
    ice_candidate_t relays[] = {candidate(RELAY, UDP, IPV4, 9, 10), candidate(RELAY, UDP, IPV4, 10, 30)};
    agent_t agent = agent_of(mini, 2, relays, 2);
    assert(agent.candidate_pairs_num == 2);
    /* A pair that sends from the socket is not a relay pair. */
    assert(pair_type(&agent, &mini[0], &relays[0]) == HOST);
    assert(pair_type(&agent, &mini[0], &relays[1]) == HOST);
    assert(pair_type(&agent, &mini[1], &relays[0]) == ICE_CANDIDATE_TYPE_NONE);
    /* Highest priority first. */
    assert(sorted[0]->remote == &relays[1] && sorted[1]->remote == &relays[0]);
    /* A second pass, as after each new remote candidate, adds nothing. */
    jetkvm_ice_pair_candidates(&agent);
    assert(agent.candidate_pairs_num == 2);

    /* A browser with every type: one pair for each of its candidates. */
    ice_candidate_t browser[] = {candidate(HOST, UDP, IPV4, 5, 2000), candidate(SRFLX, UDP, IPV4, 6, 1500),
                                 candidate(RELAY, UDP, IPV4, 7, 10)};
    agent = agent_of(mini, 2, browser, 3);
    assert(agent.candidate_pairs_num == 3);
    assert(pair_type(&agent, &mini[0], &browser[0]) == HOST);
    assert(pair_type(&agent, &mini[1], &browser[1]) == SRFLX);
    assert(pair_type(&agent, &mini[0], &browser[2]) == HOST);

    /* An agent with its own TURN allocation sends to a relay candidate
     * through the allocation and from the socket. */
    ice_candidate_t allocated[] = {candidate(HOST, UDP, IPV4, 1, 2000), candidate(RELAY, UDP, IPV4, 3, 5)};
    agent = agent_of(allocated, 2, relays, 1);
    assert(agent.candidate_pairs_num == 2);
    assert(pair_type(&agent, &allocated[1], &relays[0]) == RELAY);
    assert(pair_type(&agent, &allocated[0], &relays[0]) == HOST);

    /* No pair across address families or transports. */
    ice_candidate_t relay6 = candidate(RELAY, UDP, IPV6, 9, 10);
    agent = agent_of(mini, 2, &relay6, 1);
    assert(agent.candidate_pairs_num == 0);
    ice_candidate_t tcp_host = candidate(HOST, TCP, IPV4, 1, 2000);
    agent = agent_of(&tcp_host, 1, relays, 1);
    assert(agent.candidate_pairs_num == 0);
    ice_candidate_t tcp_relay = candidate(RELAY, TCP, IPV4, 9, 10);
    agent = agent_of(mini, 2, &tcp_relay, 1);
    assert(agent.candidate_pairs_num == 0);
    agent = agent_of(&tcp_host, 1, &tcp_relay, 1);
    assert(agent.candidate_pairs_num == 0);
    return 0;
}
'''


class EspPeerIcePairingTest(unittest.TestCase):
    def test_pairs_of_the_agent(self) -> None:
        agent = (CORE / 'agent.c').read_text()
        ice = (CORE / 'ice.h').read_text()
        address = typedef((ROOT / 'third_party/esp_peer/include/esp_peer_types.h').read_text(), 'esp_peer_addr_t')
        source = '\n'.join([
            '#include <assert.h>\n#include <stdbool.h>\n#include <stdint.h>\n#include <string.h>\n#include <time.h>',
            '#define PRIVATE static',
            address,
            *(typedef(ice, name) for name in ('ice_candidate_type_t', 'ice_candidate_transport_t', 'ice_candidate_t')),
            *(typedef(agent, name) for name in ('ice_candidate_state_t', 'ice_candidate_pair_t')),
            HARNESS,
            *(function(agent, name) for name in ('agent_pair_exists', 'agent_candidates_pair', 'jetkvm_ice_pair_candidates')),
            TESTS,
        ])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(source)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            str(path / 'test.c'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
