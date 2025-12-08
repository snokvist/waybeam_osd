One-shot values/texts
./waybeam send --dest 192.168.2.20 \
  --value 0=-52 --value 1=-52 \
  --text 0=Trollvinter

Disable assets
./waybeam send --dest 192.168.2.20 \
  --asset id=0,enabled=false \
  --asset id=1,enabled=false \
  --asset id=2,enabled=false

Merge same-id asset specs
./waybeam send --dest 192.168.2.20 \
  --asset id=0,x=50,y=50 \
  --asset id=0,min=-80,max=-30,bar_color=0x00FF00

Print JSON only
./waybeam send --dest 192.168.2.20 \
  --value 0=-52 --text 0=Trollvinter \
  --print-json

JSON Lines with one-time baseline
./waybeam send --dest 192.168.2.20 \
  --asset id=0,min=-80,max=-30 \
  --stdin < updates.jsonl

Watch with explicit indices
./waybeam watch --dest 192.168.2.20 --interval 16 \
  --value-index 0 --value-index 1 --value-index 2 --value-index 3 \
  --text-index 0 \
  wlx40a5ef2f2308
