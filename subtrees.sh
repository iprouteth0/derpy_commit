#! /bin/bash

git subtree add --prefix techpack/audio https://github.com/MotorolaMobilityLLC/vendor-qcom-opensource-audio-kernel -b MMI-QZBS30.Q4-43-43-2  a6467281df5a2b08f45787fc84dc76cad073ace6

git subtree add --prefix techpack/video https://github.com/MotorolaMobilityLLC/kernel-msm-4.19-techpack-video -b MMI-QZBS30.Q4-43-43-2  2f9db4fb5ec2054f9c36c8829439d06afb7d081a

git subtree add --prefix techpack/display https://github.com/MotorolaMobilityLLC/kernel-msm-4.19-techpack-display -b MMI-QZBS30.Q4-43-43-2 d8f824ac71ffa10f49700b0a66e264056e27f104

git subtree add --prefix techpack/camera https://github.com/MotorolaMobilityLLC/kernel-msm-4.19-techpack-camera -b MMI-QZBS30.Q4-43-43-2  abf6426335d41c278db1a680cfa3c7c1ac6f181e

git subtree add --prefix drivers/staging/qcacld-3.0 https://github.com/MotorolaMobilityLLC/vendor-qcom-opensource-wlan-qcacld-3.0 -b MMI-QZBS30.Q4-43-43-2t 1c77318c98c79f42c00797dd731f1097456201e3

git subtree add --prefix drivers/staging/fw-api https://github.com/MotorolaMobilityLLC/vendor-qcom-opensource-wlan-fw-api -b MMI-QZBS30.Q4-43-43-2  2cf02cefafb8e3167bb76c34905c9048b5f330ec

git subtree add --prefix drivers/staging/qca-wifi-host-cmn https://github.com/MotorolaMobilityLLC/vendor-qcom-opensource-wlan-qca-wifi-host-cmn -b MMI-QZBS30.Q4-43-43-2  c4418e2883bd603c829d5386983211efdf0e8968
