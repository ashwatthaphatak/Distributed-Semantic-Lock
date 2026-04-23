LOCUST UI -> http://localhost:8089/

TEST CASE 1 :

DSCC_PROXY=<tailscale-IP-address>:50050 QDRANT_HOST=<tailscale-IP-address> scripts/compare_baseline.sh -d 1m -u 20 -r 5 -w

TEST CASE 2 :
DSCC_PROXY=100.105.11.115:50050 locust -f locustfile.py
