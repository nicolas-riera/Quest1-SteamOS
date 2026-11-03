# pt-undo.sh: back to the default camera mode (no roomview) after roomview tests
systemctl stop quest1-steamvr
rm -f /etc/quest1-camera
python3 - <<"PY"
import json
p = "/home/steamos/.local/share/Steam/config/steamvr.vrsettings"
d = json.load(open(p))
c = d.get("camera", {})
for k in ("enableConstructRoomView", "forceSceneAlphaBlending"):
    c.pop(k, None)
c["roomView"] = 0
json.dump(d, open(p, "w"), indent=3)
PY
chown steamos:steamos /home/steamos/.local/share/Steam/config/steamvr.vrsettings
systemctl start quest1-steamvr
