// |reftest| skip -- class-static-methods-private is not supported
// This file was procedurally generated from the following sources:
// - src/accessor-names/private-name-underscore.case
// - src/accessor-names/private/cls-private-expr-static.template
/*---
description: Private IdentifierName - _ (Class expression, static private method)
features: [class-static-methods-private]
flags: [generated]
info: |
    ClassElement :
      MethodDefinition
      static MethodDefinition
      FieldDefinition ;
      ;

    MethodDefinition :
      get ClassElementName () { FunctionBody }
      set ClassElementName ( PropertySetParameterList ){ FunctionBody }

    ClassElementName :
      PropertyName
      PrivateName

    PrivateName ::
      # IdentifierName

    Initializer :
      = AssignmentExpression

    IdentifierName ::
      IdentifierStart
      IdentifierName IdentifierPart

    IdentifierStart ::
      UnicodeIDStart
      $
      _
      \ UnicodeEscapeSequence

    IdentifierPart::
      UnicodeIDContinue
      $
      \ UnicodeEscapeSequence
      <ZWNJ> <ZWJ>

    UnicodeIDStart::
      any Unicode code point with the Unicode property "ID_Start"

    UnicodeIDContinue::
      any Unicode code point with the Unicode property "ID_Continue"


    NOTE 3
    The sets of code points with Unicode properties "ID_Start" and
    "ID_Continue" include, respectively, the code points with Unicode
    properties "Other_ID_Start" and "Other_ID_Continue".

---*/

var stringSet;

var C = class {
  static get #_() { return 'get string'; }
  static set #_(param) { stringSet = param; }

  static getPrivateReference() {
    return this.#_;
  }

  static setPrivateReference(value) {
    this.#_ = value;
  }
};


assert.sameValue(C.getPrivateReference(), 'get string');

C.setPrivateReference('set string');
assert.sameValue(stringSet, 'set string');

reportCompare(0, 0);
