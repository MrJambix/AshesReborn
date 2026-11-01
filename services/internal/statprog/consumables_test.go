// SPDX-License-Identifier: AGPL-3.0-only
package statprog

import (
	"fmt"
	"testing"

	"aocrop/services/internal/aocexpr"
)

type potionState struct{ aocexpr.NoState }

func (potionState) ConsumedItemStat(ref aocexpr.Ref) float64 {
	if ref.Guid == 1001 {
		return 37.5
	}
	return 0
}

func TestConsumablePotencyAndClassReads(t *testing.T) {
	ref := fmt.Sprintf("$#%d:1001$", TypeStatDef)
	expressions := []Expr{
		{What: "item potency", Text: "const item = GetAbilityItemInstance(); GetItemInstanceStat(item, " + ref + ") + GetConsumedItemStat(" + ref + ")"},
		{What: "caster class", Text: "SelectFloat(GetPrimaryArchetype(GetOwner()) == ECharacterClass::Ranger, 10.0, 20.0)"},
		{What: "self target class", Text: "SelectFloat(GetPrimaryArchetype(GetTarget()) == ECharacterClass::Bard, 1.0, 0.0)"},
	}
	for _, mode := range []Mode{ModeFloat32, ModeDouble, ModeSingle} {
		bundle, compiled, err := CompileExprs(TinyGraph(false), Options{Mode: mode}, expressions)
		if err != nil {
			t.Fatal(err)
		}
		for _, result := range compiled {
			if result.Program < 0 {
				t.Fatalf("consumable expression did not compile: %+v", result)
			}
		}
		for _, class := range []int{1, 2} {
			input := tinyInputs(class, 11, false)
			state := potionState{}
			run, err := bundle.Run(input, state)
			if err != nil {
				t.Fatal(err)
			}
			evaluator, err := bundle.NewEvaluator(run, input, state)
			if err != nil {
				t.Fatal(err)
			}
			reference, err := NewRefEngine(TinyGraph(false), input, state, mode)
			if err != nil {
				t.Fatal(err)
			}
			want := []float64{75, 10, 0}
			if class == 2 {
				want = []float64{75, 20, 1}
			}
			for i, expression := range compiled {
				got, err := evaluator.Eval(expression.Program, &TableHost{})
				if err != nil || got != want[i] {
					t.Fatalf("mode %v class %d %s: got %v, %v; want %v", mode, class, expressions[i].What, got, err, want[i])
				}
				refValue, err := reference.EvalExpr(expressions[i].Text, nil, &TableHost{})
				if err != nil || refValue != got {
					t.Fatalf("reference %s: got %v, %v; compiled %v", expressions[i].What, refValue, err, got)
				}
			}
		}
	}
}
