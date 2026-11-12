"""Validate resolved board builds, especially transport pins derived by Kconfig."""
from pathlib import Path
import json,re,unittest
ROOT=Path(__file__).resolve().parents[2]
class PaxBoardBuilds(unittest.TestCase):
 def config(self,board):
  folder=ROOT/'launcher'/('build_'+board)
  self.assertTrue((folder/('launcher_'+board+'.bin')).is_file(),f'Build {board} first')
  description=json.loads((folder/'project_description.json').read_text())
  self.assertEqual(description['project_name'],'launcher_'+board)
  text=(ROOT/'launcher'/('sdkconfig.'+board)).read_text()
  return dict(re.findall(r'^CONFIG_(\w+)=(.*)$',text,re.M))
 def test_aio_resolved_pins(self):
  s=self.config('elecrow_p4_aio')
  self.assertEqual(s.get('ESP_HOSTED_SPI_HOST_INTERFACE'),'y')
  for key,value in dict(MOSI=7,MISO=9,CLK=15,CS=14,HANDSHAKE=11,DATA_READY=10,RESET_SLAVE=2).items():
   self.assertEqual(s.get('ESP_HOSTED_SPI_GPIO_'+key),str(value),key)
  self.assertEqual(s.get('ESPTOOLPY_FLASHFREQ_80M'),'y')
if __name__=='__main__':unittest.main()
