%
% (c) The GRASP/AQUA Project, Glasgow University, 1992-1998
%
\section[PrimOp]{Primitive operations (machine-level)}

\begin{code}
module PrimOp (
        PrimOp(..), allThePrimOps,
        primOpType, primOpSig,
        primOpTag, maxPrimOpTag, primOpOcc,

        tagToEnumKey,

        primOpOutOfLine, primOpCodeSize,
        primOpOkForSpeculation, primOpIsCheap,

        getPrimOpResultInfo,  PrimOpResultInfo(..),

        PrimCall(..)
    ) where

#include "HsVersions.h"

import TysPrim
import TysWiredIn

import Demand
import Var              ( TyVar )
import OccName          ( OccName, pprOccName, mkVarOccFS )
import TyCon            ( TyCon, isPrimTyCon, tyConPrimRep, PrimRep(..) )
import Type             ( Type, mkForAllTys, mkFunTy, mkFunTys, tyConAppTyCon,
                          typePrimRep )
import BasicTypes       ( Arity, TupleSort(..) )
import ForeignCall      ( CLabelString )
import Unique           ( Unique, mkPrimOpIdUnique )
import Outputable
import FastTypes
import FastString
import Module           ( PackageId )
\end{code}

%************************************************************************
%*                                                                      *
\subsection[PrimOp-datatype]{Datatype for @PrimOp@ (an enumeration)}
%*                                                                      *
%************************************************************************

These are in \tr{state-interface.verb} order.

\begin{code}

-- supplies:
-- data PrimOp = ...
#include "primop-data-decl.hs-incl"
\end{code}

Used for the Ord instance

\begin{code}
primOpTag :: PrimOp -> Int
primOpTag op = iBox (tagOf_PrimOp op)

-- supplies
-- tagOf_PrimOp :: PrimOp -> FastInt
#include "primop-tag.hs-incl"


instance Eq PrimOp where
    op1 == op2 = tagOf_PrimOp op1 ==# tagOf_PrimOp op2

instance Ord PrimOp where
    op1 <  op2 =  tagOf_PrimOp op1 <# tagOf_PrimOp op2
    op1 <= op2 =  tagOf_PrimOp op1 <=# tagOf_PrimOp op2
    op1 >= op2 =  tagOf_PrimOp op1 >=# tagOf_PrimOp op2
    op1 >  op2 =  tagOf_PrimOp op1 ># tagOf_PrimOp op2
    op1 `compare` op2 | op1 < op2  = LT
                      | op1 == op2 = EQ
                      | otherwise  = GT

instance Outputable PrimOp where
    ppr op = pprPrimOp op

instance Show PrimOp where
    showsPrec p op = showsPrecSDoc p (pprPrimOp op)
\end{code}

An @Enum@-derived list would be better; meanwhile... (ToDo)

\begin{code}
allThePrimOps :: [PrimOp]
allThePrimOps =
#include "primop-list.hs-incl"
\end{code}

\begin{code}
tagToEnumKey :: Unique
tagToEnumKey = mkPrimOpIdUnique (primOpTag TagToEnumOp)
\end{code}



%************************************************************************
%*                                                                      *
\subsection[PrimOp-info]{The essential info about each @PrimOp@}
%*                                                                      *
%************************************************************************

The @String@ in the @PrimOpInfos@ is the ``base name'' by which the user may
refer to the primitive operation.  The conventional \tr{#}-for-
unboxed ops is added on later.

The reason for the funny characters in the names is so we do not
interfere with the programmer's Haskell name spaces.

We use @PrimKinds@ for the ``type'' information, because they're
(slightly) more convenient to use than @TyCons@.
\begin{code}
data PrimOpInfo
  = Dyadic      OccName         -- string :: T -> T -> T
                Type
  | Monadic     OccName         -- string :: T -> T
                Type
  | Compare     OccName         -- string :: T -> T -> Bool
                Type

  | GenPrimOp   OccName         -- string :: \/a1..an . T1 -> .. -> Tk -> T
                [TyVar]
                [Type]
                Type

mkDyadic, mkMonadic, mkCompare :: FastString -> Type -> PrimOpInfo
mkDyadic str  ty = Dyadic  (mkVarOccFS str) ty
mkMonadic str ty = Monadic (mkVarOccFS str) ty
mkCompare str ty = Compare (mkVarOccFS str) ty

mkGenPrimOp :: FastString -> [TyVar] -> [Type] -> Type -> PrimOpInfo
mkGenPrimOp str tvs tys ty = GenPrimOp (mkVarOccFS str) tvs tys ty
\end{code}

%************************************************************************
%*                                                                      *
\subsubsection{Strictness}
%*                                                                      *
%************************************************************************

Not all primops are strict!

\begin{code}
primOpStrictness :: PrimOp -> Arity -> StrictSig
        -- See Demand.StrictnessInfo for discussion of what the results
        -- The arity should be the arity of the primop; that's why
        -- this function isn't exported.
#include "primop-strictness.hs-incl"
\end{code}

%************************************************************************
%*                                                                      *
\subsubsection[PrimOp-comparison]{PrimOpInfo basic comparison ops}
%*                                                                      *
%************************************************************************

@primOpInfo@ gives all essential information (from which everything
else, notably a type, can be constructed) for each @PrimOp@.

\begin{code}
primOpInfo :: PrimOp -> PrimOpInfo
#include "primop-primop-info.hs-incl"
\end{code}

Here are a load of comments from the old primOp info:

A @Word#@ is an unsigned @Int#@.

@decodeFloat#@ is given w/ Integer-stuff (it's similar).

@decodeDouble#@ is given w/ Integer-stuff (it's similar).

Decoding of floating-point numbers is sorta Integer-related.  Encoding
is done with plain ccalls now (see PrelNumExtra.lhs).

A @Weak@ Pointer is created by the @mkWeak#@ primitive:

        mkWeak# :: k -> v -> f -> State# RealWorld
                        -> (# State# RealWorld, Weak# v #)

In practice, you'll use the higher-level

        data Weak v = Weak# v
        mkWeak :: k -> v -> IO () -> IO (Weak v)

The following operation dereferences a weak pointer.  The weak pointer
may have been finalized, so the operation returns a result code which
must be inspected before looking at the dereferenced value.

        deRefWeak# :: Weak# v -> State# RealWorld ->
                        (# State# RealWorld, v, Int# #)

Only look at v if the Int# returned is /= 0 !!

The higher-level op is

        deRefWeak :: Weak v -> IO (Maybe v)

Weak pointers can be finalized early by using the finalize# operation:

        finalizeWeak# :: Weak# v -> State# RealWorld ->
                           (# State# RealWorld, Int#, IO () #)

The Int# returned is either

        0 if the weak pointer has already been finalized, or it has no
          finalizer (the third component is then invalid).

        1 if the weak pointer is still alive, with the finalizer returned
          as the third component.

A {\em stable name/pointer} is an index into a table of stable name
entries.  Since the garbage collector is told about stable pointers,
it is safe to pass a stable pointer to external systems such as C
routines.

\begin{verbatim}
makeStablePtr#  :: a -> State# RealWorld -> (# State# RealWorld, StablePtr# a #)
freeStablePtr   :: StablePtr# a -> State# RealWorld -> State# RealWorld
deRefStablePtr# :: StablePtr# a -> State# RealWorld -> (# State# RealWorld, a #)
eqStablePtr#    :: StablePtr# a -> StablePtr# a -> Int#
\end{verbatim}

It may seem a bit surprising that @makeStablePtr#@ is a @IO@
operation since it doesn't (directly) involve IO operations.  The
reason is that if some optimisation pass decided to duplicate calls to
@makeStablePtr#@ and we only pass one of the stable pointers over, a
massive space leak can result.  Putting it into the IO monad
prevents this.  (Another reason for putting them in a monad is to
ensure correct sequencing wrt the side-effecting @freeStablePtr@
operation.)

An important property of stable pointers is that if you call
makeStablePtr# twice on the same object you get the same stable
pointer back.

Note that we can implement @freeStablePtr#@ using @_ccall_@ (and,
besides, it's not likely to be used from Haskell) so it's not a
primop.

Question: Why @RealWorld@ - won't any instance of @_ST@ do the job? [ADR]

Stable Names
~~~~~~~~~~~~

A stable name is like a stable pointer, but with three important differences:

        (a) You can't deRef one to get back to the original object.
        (b) You can convert one to an Int.
        (c) You don't need to 'freeStableName'

The existence of a stable name doesn't guarantee to keep the object it
points to alive (unlike a stable pointer), hence (a).

Invariants:

        (a) makeStableName always returns the same value for a given
            object (same as stable pointers).

        (b) if two stable names are equal, it implies that the objects
            from which they were created were the same.

        (c) stableNameToInt always returns the same Int for a given
            stable name.


-- HWL: The first 4 Int# in all par... annotations denote:
--   name, granularity info, size of result, degree of parallelism
--      Same  structure as _seq_ i.e. returns Int#
-- KSW: v, the second arg in parAt# and parAtForNow#, is used only to determine
--   `the processor containing the expression v'; it is not evaluated

These primops are pretty wierd.

        dataToTag# :: a -> Int    (arg must be an evaluated data type)
        tagToEnum# :: Int -> a    (result type must be an enumerated type)

The constraints aren't currently checked by the front end, but the
code generator will fall over if they aren't satisfied.

%************************************************************************
%*                                                                      *
            Which PrimOps are out-of-line
%*                                                                      *
%************************************************************************

Some PrimOps need to be called out-of-line because they either need to
perform a heap check or they block.


\begin{code}
primOpOutOfLine :: PrimOp -> Bool
#include "primop-out-of-line.hs-incl"
\end{code}


%************************************************************************
%*                                                                      *
            Failure and side effects
%*                                                                      *
%************************************************************************

Note [PrimOp can_fail and has_side_effects]
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * A primop that is neither can_fail nor has_side_effects can be
   executed speculatively, any number of times

 * A primop that is marked can_fail cannot be executed speculatively,
   (becuase the might provoke the failure), but it can be repeated.
   Why would you want to do that? Perhaps it might enable some
   eta-expansion, if you can prove that the lambda is definitely
   applied at least once. I guess we don't currently do that.

 * A primop that is marked has_side_effects can be neither speculated
   nor repeated; it must be executed exactly the right number of
   times.

So has_side_effects implies can_fail.  We don't currently exploit
the case of primops that can_fail but do not have_side_effects.

Note [primOpOkForSpeculation]
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Sometimes we may choose to execute a PrimOp even though it isn't
certain that its result will be required; ie execute them
``speculatively''.  The same thing as ``cheap eagerness.'' Usually
this is OK, because PrimOps are usually cheap, but it isn't OK for
  * PrimOps that are expensive 
  * PrimOps which can fail
  * PrimOps that have side effects

Ok-for-speculation also means that it's ok *not* to execute the
primop. For example
        case op a b of
          r -> 3
Here the result is not used, so we can discard the primop.  Anything
that has side effects mustn't be dicarded in this way, of course!

See also @primOpIsCheap@ (below).

Note [primOpHasSideEffects]
~~~~~~~~~~~~~~~~~~~~~~~~~~~
Some primops have side-effects and so, for example, must not be
duplicated.

This predicate means a little more than just "modifies the state of
the world".  What it really means is "it cosumes the state on its
input".  To see what this means, consider

 let
     t = case readMutVar# v s0 of (# s1, x #) -> (S# s1, x)
     y = case t of (s,x) -> x
 in
     ... y ... y ...

Now, this is part of an ST or IO thread, so we are guaranteed by
construction that the program uses the state in a single-threaded way.
Whenever the state resulting from the readMutVar# is demanded, the
readMutVar# will be performed, and it will be ordered correctly with
respect to other operations in the monad.

But there's another way this could go wrong: GHC can inline t into y,
and inline y.  Then although the original readMutVar# will still be
correctly ordered with respect to the other operations, there will be
one or more extra readMutVar#s performed later, possibly out-of-order.
This really happened; see #3207.

The property we need to capture about readMutVar# is that it consumes
the State# value on its input.  We must retain the linearity of the
State#.

Our fix for this is to declare any primop that must be used linearly
as having side-effects.  When primOpHasSideEffects is True,
primOpOkForSpeculation will be False, and hence primOpIsCheap will
also be False, and applications of the primop will never be
duplicated.

\begin{code}
primOpHasSideEffects :: PrimOp -> Bool
#include "primop-has-side-effects.hs-incl"

primOpCanFail :: PrimOp -> Bool
#include "primop-can-fail.hs-incl"

primOpOkForSpeculation :: PrimOp -> Bool
  -- See Note [primOpOkForSpeculation]
  -- See comments with CoreUtils.exprOkForSpeculation
primOpOkForSpeculation op
  = not (primOpHasSideEffects op || primOpOutOfLine op || primOpCanFail op)
\end{code}


primOpIsCheap
~~~~~~~~~~~~~
@primOpIsCheap@, as used in \tr{SimplUtils.lhs}.  For now (HACK
WARNING), we just borrow some other predicates for a
what-should-be-good-enough test.  "Cheap" means willing to call it more
than once, and/or push it inside a lambda.  The latter could change the
behaviour of 'seq' for primops that can fail, so we don't treat them as cheap.

\begin{code}
primOpIsCheap :: PrimOp -> Bool
primOpIsCheap op = primOpOkForSpeculation op
-- In March 2001, we changed this to
--      primOpIsCheap op = False
-- thereby making *no* primops seem cheap.  But this killed eta
-- expansion on case (x ==# y) of True -> \s -> ...
-- which is bad.  In particular a loop like
--      doLoop n = loop 0
--     where
--         loop i | i == n    = return ()
--                | otherwise = bar i >> loop (i+1)
-- allocated a closure every time round because it doesn't eta expand.
--
-- The problem that originally gave rise to the change was
--      let x = a +# b *# c in x +# x
-- were we don't want to inline x. But primopIsCheap doesn't control
-- that (it's exprIsDupable that does) so the problem doesn't occur
-- even if primOpIsCheap sometimes says 'True'.
\end{code}


%************************************************************************
%*                                                                      *
               PrimOp code size
%*                                                                      *
%************************************************************************

primOpCodeSize
~~~~~~~~~~~~~~
Gives an indication of the code size of a primop, for the purposes of
calculating unfolding sizes; see CoreUnfold.sizeExpr.

\begin{code}
primOpCodeSize :: PrimOp -> Int
#include "primop-code-size.hs-incl"

primOpCodeSizeDefault :: Int
primOpCodeSizeDefault = 1
  -- CoreUnfold.primOpSize already takes into account primOpOutOfLine
  -- and adds some further costs for the args in that case.

primOpCodeSizeForeignCall :: Int
primOpCodeSizeForeignCall = 4
\end{code}


%************************************************************************
%*                                                                      *
               PrimOp types
%*                                                                      *
%************************************************************************

\begin{code}
primOpType :: PrimOp -> Type  -- you may want to use primOpSig instead
primOpType op
  = case primOpInfo op of
    Dyadic  _occ ty -> dyadic_fun_ty ty
    Monadic _occ ty -> monadic_fun_ty ty
    Compare _occ ty -> compare_fun_ty ty

    GenPrimOp _occ tyvars arg_tys res_ty ->
        mkForAllTys tyvars (mkFunTys arg_tys res_ty)

primOpOcc :: PrimOp -> OccName
primOpOcc op = case primOpInfo op of
               Dyadic    occ _     -> occ
               Monadic   occ _     -> occ
               Compare   occ _     -> occ
               GenPrimOp occ _ _ _ -> occ

-- primOpSig is like primOpType but gives the result split apart:
-- (type variables, argument types, result type)
-- It also gives arity, strictness info

primOpSig :: PrimOp -> ([TyVar], [Type], Type, Arity, StrictSig)
primOpSig op
  = (tyvars, arg_tys, res_ty, arity, primOpStrictness op arity)
  where
    arity = length arg_tys
    (tyvars, arg_tys, res_ty)
      = case (primOpInfo op) of
        Monadic   _occ ty                    -> ([],     [ty],    ty    )
        Dyadic    _occ ty                    -> ([],     [ty,ty], ty    )
        Compare   _occ ty                    -> ([],     [ty,ty], boolTy)
        GenPrimOp _occ tyvars arg_tys res_ty -> (tyvars, arg_tys, res_ty)
\end{code}

\begin{code}
data PrimOpResultInfo
  = ReturnsPrim     PrimRep
  | ReturnsAlg      TyCon

-- Some PrimOps need not return a manifest primitive or algebraic value
-- (i.e. they might return a polymorphic value).  These PrimOps *must*
-- be out of line, or the code generator won't work.

getPrimOpResultInfo :: PrimOp -> PrimOpResultInfo
getPrimOpResultInfo op
  = case (primOpInfo op) of
      Dyadic  _ ty                        -> ReturnsPrim (typePrimRep ty)
      Monadic _ ty                        -> ReturnsPrim (typePrimRep ty)
      Compare _ _                         -> ReturnsAlg boolTyCon
      GenPrimOp _ _ _ ty | isPrimTyCon tc -> ReturnsPrim (tyConPrimRep tc)
                         | otherwise      -> ReturnsAlg tc
                         where
                           tc = tyConAppTyCon ty
                        -- All primops return a tycon-app result
                        -- The tycon can be an unboxed tuple, though, which
                        -- gives rise to a ReturnAlg
\end{code}

We do not currently make use of whether primops are commutable.

We used to try to move constants to the right hand side for strength
reduction.

\begin{code}
{-
commutableOp :: PrimOp -> Bool
#include "primop-commutable.hs-incl"
-}
\end{code}

Utils:
\begin{code}
dyadic_fun_ty, monadic_fun_ty, compare_fun_ty :: Type -> Type
dyadic_fun_ty  ty = mkFunTys [ty, ty] ty
monadic_fun_ty ty = mkFunTy  ty ty
compare_fun_ty ty = mkFunTys [ty, ty] boolTy
\end{code}

Output stuff:
\begin{code}
pprPrimOp  :: PrimOp -> SDoc
pprPrimOp other_op = pprOccName (primOpOcc other_op)
\end{code}


%************************************************************************
%*                                                                      *
\subsubsection[PrimCall]{User-imported primitive calls}
%*                                                                      *
%************************************************************************

\begin{code}
data PrimCall = PrimCall CLabelString PackageId

instance Outputable PrimCall where
  ppr (PrimCall lbl pkgId)
        = text "__primcall" <+> ppr pkgId <+> ppr lbl

\end{code}
