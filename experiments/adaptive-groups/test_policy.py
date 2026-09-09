#!/usr/bin/env python3
"""Pure policy model. Native runner independently checks the resulting DSP plans."""
from __future__ import annotations
from dataclasses import dataclass
import unittest

@dataclass(frozen=True, order=True)
class Unit:
    members: tuple[int, ...]

class PolicyError(ValueError): pass

class Policy:
    def __init__(self, count: int, width: int, feedback: bool=False, parallel: bool=False):
        if count <= 0 or width <= 0 or count % width: raise PolicyError('shape')
        self.feedback=feedback; self.parallel=parallel
        self.units=[Unit(tuple(range(i, i+width))) for i in range(1,count+1,width)]
        if feedback and len(self.units)!=1: raise PolicyError('feedback SCC must be whole')
    def owner(self, member:int)->int:
        for i,u in enumerate(self.units):
            if member in u.members:return i
        raise PolicyError('member')
    def expose(self, member:int)->tuple[Unit,...]:
        i=self.owner(member);u=self.units[i]
        if self.feedback: raise PolicyError('cannot cut feedback SCC')
        if self.parallel: raise PolicyError('partial parallel exposure unsupported')
        at=u.members.index(member)
        pieces=[]
        if at: pieces.append(Unit(u.members[:at]))
        pieces.append(Unit((member,)))
        if at+1<len(u.members): pieces.append(Unit(u.members[at+1:]))
        self.units[i:i+1]=pieces
        return tuple(pieces)
    def connect(self, source:int,target:int)->str:
        a,b=self.owner(source),self.owner(target)
        if a==b:
            u=self.units[a]
            # Endpoint edits within a compiled unit require opening both endpoints.
            if len(u.members)>1:
                self.expose(source)
                if self.owner(target)==self.owner(source) and target!=source:self.expose(target)
                elif len(self.units[self.owner(target)].members)>1:self.expose(target)
                return 'split'
            return 'internal-singleton'
        return 'external'
    def undo(self)->str:
        # Interactive undo does not eagerly merge/refuse running units.
        return 'retain-open-boundaries'

class Tests(unittest.TestCase):
    def test_initial_groups(self):
        self.assertEqual([u.members for u in Policy(16,4).units],[(1,2,3,4),(5,6,7,8),(9,10,11,12),(13,14,15,16)])
    def test_hidden_member_splits_only_owner(self):
        p=Policy(16,4); before=p.units[:]; p.expose(6)
        self.assertEqual([u.members for u in p.units],[(1,2,3,4),(5,),(6,),(7,8),(9,10,11,12),(13,14,15,16)])
        self.assertEqual(p.units[0],before[0]); self.assertEqual(p.units[-2:],before[-2:])
    def test_cross_group_does_not_merge(self):
        p=Policy(16,4);before=p.units[:];self.assertEqual(p.connect(4,5),'external');self.assertEqual(p.units,before)
    def test_internal_endpoint_opens_boundaries(self):
        p=Policy(8,4);self.assertEqual(p.connect(2,3),'split')
        self.assertEqual([u.members for u in p.units],[(1,),(2,),(3,),(4,),(5,6,7,8)])
    def test_undo_does_not_refuse(self):
        p=Policy(8,4);p.expose(2);state=p.units[:];self.assertEqual(p.undo(),'retain-open-boundaries');self.assertEqual(p.units,state)
    def test_feedback_cut_rejected(self):
        with self.assertRaises(PolicyError):Policy(8,8,feedback=True).expose(4)
    def test_parallel_partial_exposure_rejected(self):
        with self.assertRaises(PolicyError):Policy(8,4,parallel=True).expose(2)
    def test_bad_shapes(self):
        for args in [(0,4),(8,0),(7,4)]:
            with self.assertRaises(PolicyError):Policy(*args)

if __name__=='__main__':unittest.main(verbosity=2)
