#!/usr/bin/env python3
"""Observer regressions. Synthetic policies are NOT native scheduling results."""
import copy,unittest
from policy_evidence import validate

def fixture(budget='geometry'):
    case=dict(budget=budget,block=128,voices=1,grain=4)
    old=512/44.1;period=128/48
    def policy(caller):
        constraint=old if caller or budget=='startup' else period
        return dict(result=0,default=0,period=period if budget=='periodic' and not caller else 0,
                    computation=constraint*.5 if budget=='periodic' and not caller else constraint,constraint=constraint)
    rows=[dict(policy(False),sample=0,batch=0,caller=0),dict(policy(True),sample=128,batch=0,caller=1)]
    deadlines=[dict(sample=i*128,frames=128,worker_tc_jobs=int(i==0),worker_other_jobs=0,caller_tc_jobs=int(i==1),policy_errors=0) for i in range(2)]
    metadata=dict(context='workers',job_probe=True,worker_budget=budget,configured_player_rate=48000,
                  configured_player_frames=512 if budget=='startup' else 128,voices=4,grain=4,mach_tick_ns=1e6,
                  worker_policy_rows=2,sample_frames=256,caller_thread_policy=policy(True))
    return metadata,case,rows,deadlines

class PolicyEvidenceTests(unittest.TestCase):
    def test_final_all_caller_does_not_erase_earlier_workers(self):
        m,c,p,d=fixture();r=validate(m,c,p,d)
        self.assertEqual(r['worker_tc_jobs'],1);self.assertEqual(r['final_worker_observations'],0)
    def test_all_three_budget_contracts(self):
        for b in ('startup','geometry','periodic'):
            with self.subTest(b=b):validate(*fixture(b))
    def test_wrong_worker_budget_rejects(self):
        m,c,p,d=fixture();p[0]['constraint']=512/44.1
        with self.assertRaises(ValueError):validate(m,c,p,d)
    def test_worker_default_rejects(self):
        m,c,p,d=fixture();p[0]['default']=1
        with self.assertRaises(ValueError):validate(m,c,p,d)
    def test_read_error_rejects(self):
        m,c,p,d=fixture();p[0]['result']=5
        with self.assertRaises(ValueError):validate(m,c,p,d)
    def test_missing_record_rejects(self):
        m,c,p,d=fixture();p.pop()
        with self.assertRaises(ValueError):validate(m,c,p,d)
    def test_duplicate_record_rejects(self):
        m,c,p,d=fixture();p[1]=copy.deepcopy(p[0])
        with self.assertRaises(ValueError):validate(m,c,p,d)
    def test_counter_mismatch_rejects(self):
        m,c,p,d=fixture();d[0]['worker_tc_jobs']=0
        with self.assertRaises(ValueError):validate(m,c,p,d)
    def test_no_worker_for_whole_run_rejects(self):
        m,c,p,d=fixture();p[0]=dict(p[1],sample=0);d[0].update(worker_tc_jobs=0,caller_tc_jobs=1)
        with self.assertRaises(ValueError):validate(m,c,p,d)
    def test_caller_policy_change_rejects(self):
        m,c,p,d=fixture();m['caller_thread_policy']['constraint']=128/48
        with self.assertRaises(ValueError):validate(m,c,p,d)
    def test_nonfinite_policy_rejects(self):
        m,c,p,d=fixture();p[0]['constraint']=float('nan')
        with self.assertRaises(ValueError):validate(m,c,p,d)
if __name__=='__main__':unittest.main()
