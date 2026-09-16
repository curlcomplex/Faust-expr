"""Original diagnostic/musical OPZ programs. Raw chip fields, not TX81Z SysEx presets."""
BASE={'algorithm':5,'feedback':0,'level':.8,'op1TL':10,'op2TL':28,'op3TL':17,'op4TL':37,
      'op1Coarse':1,'op2Coarse':2,'op3Coarse':1,'op4Coarse':3,
      'op1AR':31,'op2AR':29,'op3AR':31,'op4AR':27,
      'op1D1R':15,'op2D1R':22,'op3D1R':16,'op4D1R':25,
      'op1SL':5,'op2SL':12,'op3SL':7,'op4SL':13,
      'op1D2R':4,'op2D2R':7,'op3D2R':4,'op4D2R':10,
      'op1RR':8,'op2RR':10,'op3RR':8,'op4RR':10}
PROGRAMS=[
 ('01-independent-envelope-bass',BASE|{'algorithm':1,'feedback':2,'op1Wave':6,'op2Wave':1,'op2TL':24,'op3TL':42,'op4TL':48},[36,36,43,39,36,46,43,34],.27),
 ('02-dual-pair-keys',BASE|{'op2Wave':7,'op4Wave':3,'op3DT1':2,'op4Fine':3},[48,55,60,63,58,55,51,46],.36),
 ('03-oracle-fixed-mode-study',BASE|{'algorithm':5,'op2Mode':1,'op2Coarse':8,'op2Fine':7,'op2Range':4,'op2TL':23,'op4Mode':1,'op4Coarse':6,'op4Fine':0,'op4Range':3,'op4TL':26,'op1RR':6,'op3RR':6},[55,60,62,67,65,62,58,53],.42),
 ('04-coarse-detune-bells',BASE|{'algorithm':6,'op4DT2':2,'op4TL':24,'op4D1R':13,'op1SL':2,'op2SL':4,'op3SL':6,'op1RR':5,'op2RR':5,'op3RR':5},[60,67,63,72,70,67,63,58],.48),
 ('05-feedback-reed',BASE|{'algorithm':1,'feedback':6,'op1Wave':0,'op2TL':26,'op3TL':28,'op4TL':21,'op4Coarse':1,'op4D1R':18,'op4SL':7},[48,50,53,55,58,55,53,50],.34),
]
