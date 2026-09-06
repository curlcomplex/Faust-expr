"""Small Rayleigh--Ritz shallow-shell model. SI units; no fitted audio samples.

Clamped 3 mm mounting-hole radius, free rim, tapered thickness. In-plane
coordinates are statically condensed before solving transverse eigenmodes.
The realtime model interpolates nine bell geometries in a limited basis.
"""
from __future__ import annotations
import numpy as np
from numpy.polynomial import Polynomial, Legendre
from scipy.linalg import eigh
from scipy.optimize import linear_sum_assignment
from math import comb

RADIUS = 0.25
THICKNESS = 0.0012
DENSITY = 8800.0
YOUNG = 100e9
POISSON = 0.34
HOLE = 0.012
BELL_RADII = (0.08, 0.24, 0.50)
BELL_HEIGHTS = (0.0, 0.07, 0.14)
RADIAL_INDICES = (0, 1, 3, 5, 8, 11)
RADIAL_MODES = len(RADIAL_INDICES)
ANGULAR_ORDERS = 14
BASIS_SIZE = 16
SAMPLES = 41

def raw_basis(descriptor, r, derivative=0):
    envelope, legs = descriptor
    r=np.asarray(r)
    scale=2/(1-HOLE); x=(2*r-1-HOLE)/(1-HOLE)
    # Direct Legendre recurrences avoid high-degree monomial cancellation.
    return np.array([sum(comb(derivative,k)*envelope.deriv(k)(r)
        *p.deriv(derivative-k)(x)*scale**(derivative-k)
        for k in range(derivative+1)) for p in legs]).T

def basis(m: int, size: int, power: int, r: np.ndarray):
    envelope=Polynomial([-HOLE,1])**power * Polynomial([0,1])**max(0,m-power)
    descriptor=(envelope,[Legendre.basis(j) for j in range(size)])
    _, transform=np.linalg.qr(raw_basis(descriptor,r))
    return descriptor,np.linalg.inv(transform)

def evaluate(descriptor, transform, r, derivative=0):
    return raw_basis(descriptor,r,derivative) @ transform

def shell_modes(m: int, bell_radius: float, bell_height: float):
    x, wt = np.polynomial.legendre.leggauss(112)
    r = (x+1)*(1-HOLE)/2+HOLE
    angular = 2*np.pi if m == 0 else np.pi
    area = wt*(1-HOLE)/2*r*RADIUS**2*angular
    h = THICKNESS*(1+1.6*(1-r)**2)
    slope = -0.07*r - 4*bell_height*r**3/bell_radius**4*np.exp(-(r/bell_radius)**4)
    wp, wq = basis(m, BASIS_SIZE, 2, r)
    up, uq = basis(m, BASIS_SIZE, 1, r)
    w = evaluate(wp,wq,r); wr = evaluate(wp,wq,r,1)/RADIUS
    wrr = evaluate(wp,wq,r,2)/RADIUS**2
    u = evaluate(up,uq,r); ur = evaluate(up,uq,r,1)/RADIUS
    radius = r[:,None]*RADIUS
    krr, ktt = wrr, wr/radius-m*m*w/radius**2
    krt = -m*(wr/radius-w/radius**2)
    D = YOUNG*h**3/(12*(1-POISSON**2))
    gram = lambda a,b,weight: a.T @ (weight[:,None]*b)
    kb = gram(krr,krr,area*D)+gram(ktt,ktt,area*D)
    kb += POISSON*(gram(krr,ktt,area*D)+gram(ktt,krr,area*D))
    kb += 2*(1-POISSON)*gram(krt,krt,area*D)
    zeros = np.zeros_like(w)
    ew = [slope[:,None]*wr,zeros,-m*slope[:,None]*w/radius]
    eu = [ur,u/radius,-m*u/radius]
    ev = [zeros,m*u/radius,ur-u/radius]
    groups = [ew,eu] if m == 0 else [ew,eu,ev]
    E = [np.concatenate([g[i] for g in groups],axis=1) for i in range(3)]
    A = area*YOUNG*h/(1-POISSON**2)
    km = gram(E[0],E[0],A)+gram(E[1],E[1],A)
    km += POISSON*(gram(E[0],E[1],A)+gram(E[1],E[0],A))
    km += (1-POISSON)*0.5*gram(E[2],E[2],A)
    n=BASIS_SIZE
    condensed = km[:n,:n]-km[:n,n:] @ np.linalg.solve(km[n:,n:],km[n:,:n])
    condensed = (condensed+condensed.T)/2
    mass=gram(w,w,area*DENSITY*h)
    eigen, vectors=eigh((kb+condensed+kb.T+condensed.T)/2,mass)
    if eigen[0] <= 0 or not np.isfinite(eigen).all(): raise ValueError('non-positive shell stiffness')
    q = vectors[:,RADIAL_INDICES]
    bend=np.sum(q*(kb@q),axis=0)/(2*np.pi)**2
    membrane=np.maximum(0,np.sum(q*(condensed@q),axis=0)/(2*np.pi)**2)
    sample_r=np.linspace(HOLE,1,SAMPLES)
    shapes=evaluate(wp,wq,sample_r)@q
    slope_nl=(evaluate(wp,wq,np.array([0.63]),1)@q)[0]/RADIUS
    for j in range(RADIAL_MODES):
        sign=np.sign(shapes[np.argmax(abs(shapes[:,j])),j]) or 1
        shapes[:,j]*=sign; slope_nl[j]*=sign; q[:,j]*=sign
    return {'bend':bend,'membrane':membrane,'shape':shapes,'slope':slope_nl,'mass':mass,'q':q}

def make_grid():
    result=[]
    for height in BELL_HEIGHTS:
        for radius in BELL_RADII:
            result.append([shell_modes(m,radius,height) for m in range(ANGULAR_ORDERS)])
    for m in range(ANGULAR_ORDERS):
        ref=result[4][m]
        for g in range(9):
            target=result[g][m]
            _,order=linear_sum_assignment(-abs(ref['q'].T@ref['mass']@target['q']))
            for field in ('bend','membrane','slope'):
                target[field]=target[field][order]
            target['shape']=target['shape'][:,order]
            target['q']=target['q'][:,order]
            signs=np.sign(np.sum(ref['q']*(ref['mass']@target['q']),axis=0))
            signs[signs==0]=1
            target['shape']*=signs; target['slope']*=signs; target['q']*=signs
    return result

if __name__=='__main__':
    grid=make_grid()
    for g in (0,4,8):
        f=np.concatenate([np.sqrt(v['bend']+v['membrane']) for v in grid[g]])
        print(g,'frequency range',float(f.min()),float(f.max()),'Hz')
