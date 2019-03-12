(set-logic QF_BV)
(set-info :status sat)

(declare-fun c () (_ BitVec 8))
(declare-fun s () (_ BitVec 8))
(declare-fun x () (_ BitVec 8))

(assert (= c #b00000011))
(assert (= s #b11110101))
(assert (= (bvashr x c) s))

(check-sat)

(exit)
