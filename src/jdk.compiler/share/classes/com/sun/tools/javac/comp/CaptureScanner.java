/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package com.sun.tools.javac.comp;

import com.sun.tools.javac.code.Symbol;
import com.sun.tools.javac.tree.JCTree;
import com.sun.tools.javac.tree.TreeScanner;
import com.sun.tools.javac.util.List;

import java.util.HashSet;
import java.util.Set;

import static com.sun.tools.javac.code.Kinds.Kind.MTH;
import static com.sun.tools.javac.code.Kinds.Kind.VAR;

/**
 * A visitor which collects the set of local variables "captured" by a given tree.
 */
public class CaptureScanner extends TreeScanner {

    /**
     * the owner tree
     */
    final JCTree ownerTree;

    final Set<Symbol.VarSymbol> seenVars = new HashSet<>();

    /**
     * The list of owner's variables accessed from within the local class,
     * without any duplicates.
     */
    List<Symbol.VarSymbol> fvs = List.nil();


    CaptureScanner(JCTree ownerTree) {
        this.ownerTree = ownerTree;
    }

    @Override
    public void visitIdent(JCTree.JCIdent tree) {
        Symbol sym = tree.sym;
        if (sym.kind == VAR && sym.owner.kind == MTH) {
            Symbol.VarSymbol vsym = (Symbol.VarSymbol) sym;
            if (vsym.getConstValue() == null && !seenVars.contains(vsym)) {
                addFreeVar(vsym);
            }
        }
    }

    /**
     * Add free variable to fvs list unless it is already there.
     */
    void addFreeVar(Symbol.VarSymbol v) {
        for (List<Symbol.VarSymbol> l = fvs; l.nonEmpty(); l = l.tail)
            if (l.head == v) return;
        fvs = fvs.prepend(v);
    }

    @Override
    public void visitVarDef(JCTree.JCVariableDecl tree) {
        if (tree.sym.owner.kind == MTH) {
            seenVars.add(tree.sym);
        }
        super.visitVarDef(tree);
    }

    List<Symbol.VarSymbol> analyzeCaptures() {
        scan(ownerTree);
        return fvs;
    }
}
