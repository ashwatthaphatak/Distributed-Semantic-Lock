LOCUST UI -> http://localhost:8089/
QDRANT UI -> http://localhost:6333/dashboard

For purposes of todays demo:
Ashwattha - Client
Ayush - Server


TO-DOs before demo:
- Make sure you build the entire system again on both systems inside the /build folder of the repo and not inside the /tmp/folders. The command to build entire project and demo files is
```
TO-DO
```

- Make sure tailscale set up works in class (NCSU wifi could be mess up)
- Make sure tailscale IPs are noted and that <tailscale-IP-address> is replaced with the actual IP address before demo.
- Have Locust URL in browser (Ashwattha)
- Have Qdrant URL in browser (Ayush)
- Have Lucidchart diagram pre-loaded for explanation (Ashwattha)

---

## TEST CASE 1 : (Ashwattha's Laptop on projector)

### Ashwattha

Start the embedding service. Run in root:
```
docker compose up embedding-service --no-deps
```

then to start workload generation:
```
DSCC_PROXY=<tailscale-IP-address>:50050 QDRANT_HOST=<tailscale-IP-address> scripts/compare_baseline.sh -d 1m -u 50 -r 5 -w
```

**Immediately switch to locust UI and refresh until page loads. Simultaneously load generation should begin on Ayush's Laptop**

Make sure to replace <tailscale-IP-address> with Ayush's tailnet IP.

### Ayush

To start the server and its components, run
```
DSCC_LOCK_HOLD_MS=2 docker compose -f docker-compose.server.yml up
```

Start 5 terminals, one for each dscc-node. 
```
docker compose -f ~/Desktop/Distributed-Semantic-Lock/docker-compose.server.yml logs -f --tail 20 dscc-node-1
```
---

## TEST CASE 2 & 3 & 4 (RAFT): (Ayush's Laptop on projector)

ASHWATTHA's MACHINE
```
DSCC_PROXY=<tailscale-IP-address>:50050 locust -f locustfile.py
```

Then:
- Open Locust UI
- No. of users 20
- Ramp up speed: 5
- HOST: http://<tailscale-IP-address>:50050

AYUSH's MACHINE

- Firstly `docker compose down` to stop first experiment.
Then run
```
DSCC_LOCK_HOLD_MS=100 docker compose -f docker-compose.server.yml up
```

- Make sure 5 terminals for dscc-nodes are running  
- Make sure all server components are running with `docker ps`
- After Ashwattha starts workload, show the workload generation and explain what's happening for a second. Show 5 nodes, show Qdrant UI, etc.

**Test 2**:
```
docker kill <leader-node>
```
- Show that the client does not see any difference in latency. No dip or change in p95
- Show how the leader changed. Clearly make note of who the new leader is. Imp for test 3.

**Test 3**
- Bring back the leader 
```
docker start <same node that was killed>
```
- The node should be back up. Maybe a few election terms here and there. But all should be fine. Test this in morning test run.
- Show that the leader changed for some reason. Explain why? Pre-vote mechanism. gRPC exponential backoff, gRPC channel full, etc. How etcd, CockroachDB, and Consul fix this with Pre-vote. Diego Ongaro in his thesis as an optimization

**Test 4**
- Kill 2 followers
- Show that system still works. workload is being processed
- Kill 3rd follower. System should stop processing. Locust failures should rise
- Bring back 1 follower. System should work.

## TEST CASE 5 (Ayush's machine on Projector)

Before anything. Run `docker compose down`
**ONLY TO BE RUN ON AYUSH's MACHINE**

DSLM_GAUNTLET_THETA=0.75 E2E_TEARDOWN=0 ./build/dscc-paraphrase-gauntlet-demo