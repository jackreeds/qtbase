/****************************************************************************
**
** Copyright (C) 2020 The Qt Company Ltd.
** Copyright (C) 2019 Olivier Goffart <ogoffart@woboq.com>
** Contact: https://www.qt.io/licensing/
**
** This file is part of the tools applications of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:GPL-EXCEPT$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "moc.h"
#include "generator.h"
#include "qdatetime.h"
#include "utils.h"
#include "outputrevision.h"
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qdir.h>
#include <QtCore/qjsondocument.h>

// for normalizeTypeInternal
#include <private/qmetaobject_moc_p.h>
#include <private/qduplicatetracker_p.h>

QT_BEGIN_NAMESPACE

// only moc needs this function
static QByteArray normalizeType(const QByteArray &ba)
{
    return ba.size() ? normalizeTypeInternal(ba.constBegin(), ba.constEnd()) : ba;
}

bool Moc::parseClassHead(ClassDef *def)
{
    // figure out whether this is a class declaration, or only a
    // forward or variable declaration.
    int i = 0;
    Token token;
    do {
        token = lookup(i++);
        if (token == COLON || token == LBRACE)
            break;
        if (token == SEMIC || token == RANGLE)
            return false;
    } while (token);

    if (!test(IDENTIFIER)) // typedef struct { ... }
        return false;
    QByteArray name = lexem();

    // support "class IDENT name" and "class IDENT(IDENT) name"
    // also support "class IDENT name (final|sealed|Q_DECL_FINAL)"
    if (test(LPAREN)) {
        until(RPAREN);
        if (!test(IDENTIFIER))
            return false;
        name = lexem();
    } else  if (test(IDENTIFIER)) {
        const QByteArray lex = lexem();
        if (lex != "final" && lex != "sealed" && lex != "Q_DECL_FINAL")
            name = lex;
    }

    def->qualified += name;
    while (test(SCOPE)) {
        def->qualified += lexem();
        if (test(IDENTIFIER)) {
            name = lexem();
            def->qualified += name;
        }
    }
    def->classname = name;

    if (test(IDENTIFIER)) {
        const QByteArray lex = lexem();
        if (lex != "final" && lex != "sealed" && lex != "Q_DECL_FINAL")
            return false;
    }

    if (test(COLON)) {
        do {
            test(VIRTUAL);
            FunctionDef::Access access = FunctionDef::Public;
            if (test(PRIVATE))
                access = FunctionDef::Private;
            else if (test(PROTECTED))
                access = FunctionDef::Protected;
            else
                test(PUBLIC);
            test(VIRTUAL);
            const QByteArray type = parseType().name;
            // ignore the 'class Foo : BAR(Baz)' case
            if (test(LPAREN)) {
                until(RPAREN);
            } else {
                def->superclassList += qMakePair(type, access);
            }
        } while (test(COMMA));

        if (!def->superclassList.isEmpty()
            && knownGadgets.contains(def->superclassList.constFirst().first)) {
            // Q_GADGET subclasses are treated as Q_GADGETs
            knownGadgets.insert(def->classname, def->qualified);
            knownGadgets.insert(def->qualified, def->qualified);
        }
    }
    if (!test(LBRACE))
        return false;
    def->begin = index - 1;
    bool foundRBrace = until(RBRACE);
    def->end = index;
    index = def->begin + 1;
    return foundRBrace;
}

Type Moc::parseType()
{
    Type type;
    bool hasSignedOrUnsigned = false;
    bool isVoid = false;
    type.firstToken = lookup();
    for (;;) {
        skipCxxAttributes();
        switch (next()) {
            case SIGNED:
            case UNSIGNED:
                hasSignedOrUnsigned = true;
                Q_FALLTHROUGH();
            case CONST:
            case VOLATILE:
                type.name += lexem();
                type.name += ' ';
                if (lookup(0) == VOLATILE)
                    type.isVolatile = true;
                continue;
            case Q_MOC_COMPAT_TOKEN:
            case Q_INVOKABLE_TOKEN:
            case Q_SCRIPTABLE_TOKEN:
            case Q_SIGNALS_TOKEN:
            case Q_SLOTS_TOKEN:
            case Q_SIGNAL_TOKEN:
            case Q_SLOT_TOKEN:
                type.name += lexem();
                return type;
            case NOTOKEN:
                return type;
            default:
                prev();
                break;
        }
        break;
    }

    skipCxxAttributes();
    test(ENUM) || test(CLASS) || test(STRUCT);
    for(;;) {
        skipCxxAttributes();
        switch (next()) {
        case IDENTIFIER:
            // void mySlot(unsigned myArg)
            if (hasSignedOrUnsigned) {
                prev();
                break;
            }
            Q_FALLTHROUGH();
        case CHAR:
        case SHORT:
        case INT:
        case LONG:
            type.name += lexem();
            // preserve '[unsigned] long long', 'short int', 'long int', 'long double'
            if (test(LONG) || test(INT) || test(DOUBLE)) {
                type.name += ' ';
                prev();
                continue;
            }
            break;
        case FLOAT:
        case DOUBLE:
        case VOID:
        case BOOL:
        case AUTO:
            type.name += lexem();
            isVoid |= (lookup(0) == VOID);
            break;
        case NOTOKEN:
            return type;
        default:
            prev();
            ;
        }
        if (test(LANGLE)) {
            if (type.name.isEmpty()) {
                // '<' cannot start a type
                return type;
            }
            type.name += lexemUntil(RANGLE);
        }
        if (test(SCOPE)) {
            type.name += lexem();
            type.isScoped = true;
        } else {
            break;
        }
    }
    while (test(CONST) || test(VOLATILE) || test(SIGNED) || test(UNSIGNED)
           || test(STAR) || test(AND) || test(ANDAND)) {
        type.name += ' ';
        type.name += lexem();
        if (lookup(0) == AND)
            type.referenceType = Type::Reference;
        else if (lookup(0) == ANDAND)
            type.referenceType = Type::RValueReference;
        else if (lookup(0) == STAR)
            type.referenceType = Type::Pointer;
    }
    type.rawName = type.name;
    // transform stupid things like 'const void' or 'void const' into 'void'
    if (isVoid && type.referenceType == Type::NoReference) {
        type.name = "void";
    }
    return type;
}

enum class IncludeState {
    IncludeBegin,
    IncludeEnd,
    NoInclude,
};

bool Moc::parseEnum(EnumDef *def)
{
    bool isTypdefEnum = false; // typedef enum { ... } Foo;

    if (test(CLASS) || test(STRUCT))
        def->isEnumClass = true;

    if (test(IDENTIFIER)) {
        def->name = lexem();
    } else {
        if (lookup(-1) != TYPEDEF)
            return false; // anonymous enum
        isTypdefEnum = true;
    }
    if (test(COLON)) { // C++11 strongly typed enum
        // enum Foo : unsigned long { ... };
        parseType(); //ignore the result
    }
    if (!test(LBRACE))
        return false;
    auto handleInclude = [this]() -> IncludeState  {
        bool hadIncludeBegin = false;
        if (test(MOC_INCLUDE_BEGIN)) {
            currentFilenames.push(symbol().unquotedLexem());
            // we do not return early to handle empty headers in one go
            hadIncludeBegin = true;
        }
        if (test(NOTOKEN)) {
            next(MOC_INCLUDE_END);
            currentFilenames.pop();
            return IncludeState::IncludeEnd;
        }
        if (hadIncludeBegin)
            return IncludeState::IncludeBegin;
        else
            return IncludeState::NoInclude;
    };
    do {
        if (lookup() == RBRACE) // accept trailing comma
            break;
        if ( handleInclude() == IncludeState::IncludeEnd)
            continue;
        next(IDENTIFIER);
        def->values += lexem();
        handleInclude();
        skipCxxAttributes();
    } while (test(EQ) ? until(COMMA) : test(COMMA));
    next(RBRACE);
    if (isTypdefEnum) {
        if (!test(IDENTIFIER))
            return false;
        def->name = lexem();
    }
    return true;
}

void Moc::parseFunctionArguments(FunctionDef *def)
{
    Q_UNUSED(def);
    while (hasNext()) {
        ArgumentDef  arg;
        arg.type = parseType();
        if (arg.type.name == "void")
            break;
        if (test(IDENTIFIER))
            arg.name = lexem();
        while (test(LBRACK)) {
            arg.rightType += lexemUntil(RBRACK);
        }
        if (test(CONST) || test(VOLATILE)) {
            arg.rightType += ' ';
            arg.rightType += lexem();
        }
        arg.normalizedType = normalizeType(QByteArray(arg.type.name + ' ' + arg.rightType));
        arg.typeNameForCast = normalizeType(QByteArray(noRef(arg.type.name) + "(*)" + arg.rightType));
        if (test(EQ))
            arg.isDefault = true;
        def->arguments += arg;
        if (!until(COMMA))
            break;
    }

    if (!def->arguments.isEmpty()
        && def->arguments.constLast().normalizedType == "QPrivateSignal") {
        def->arguments.removeLast();
        def->isPrivateSignal = true;
    }
    if (def->arguments.size() == 1
        && def->arguments.constLast().normalizedType == "QMethodRawArguments") {
        def->arguments.removeLast();
        def->isRawSlot = true;
    }
}

bool Moc::testFunctionAttribute(FunctionDef *def)
{
    if (index < symbols.size() && testFunctionAttribute(symbols.at(index).token, def)) {
        ++index;
        return true;
    }
    return false;
}

bool Moc::testFunctionAttribute(Token tok, FunctionDef *def)
{
    switch (tok) {
        case Q_MOC_COMPAT_TOKEN:
            def->isCompat = true;
            return true;
        case Q_INVOKABLE_TOKEN:
            def->isInvokable = true;
            return true;
        case Q_SIGNAL_TOKEN:
            def->isSignal = true;
            return true;
        case Q_SLOT_TOKEN:
            def->isSlot = true;
            return true;
        case Q_SCRIPTABLE_TOKEN:
            def->isInvokable = def->isScriptable = true;
            return true;
        default: break;
    }
    return false;
}

bool Moc::skipCxxAttributes()
{
    auto rewind = index;
    if (test(LBRACK) && test(LBRACK) && until(RBRACK) && test(RBRACK))
        return true;
    index = rewind;
    return false;
}

QTypeRevision Moc::parseRevision()
{
    next(LPAREN);
    QByteArray revisionString = lexemUntil(RPAREN);
    revisionString.remove(0, 1);
    revisionString.chop(1);
    const QList<QByteArray> majorMinor = revisionString.split(',');
    switch (majorMinor.length()) {
    case 1: {
        bool ok = false;
        const int revision = revisionString.toInt(&ok);
        if (!ok || !QTypeRevision::isValidSegment(revision))
            error("Invalid revision");
        return QTypeRevision::fromMinorVersion(revision);
    }
    case 2: { // major.minor
        bool ok = false;
        const int major = majorMinor[0].toInt(&ok);
        if (!ok || !QTypeRevision::isValidSegment(major))
            error("Invalid major version");
        const int minor = majorMinor[1].toInt(&ok);
        if (!ok || !QTypeRevision::isValidSegment(minor))
            error("Invalid minor version");
        return QTypeRevision::fromVersion(major, minor);
    }
    default:
        error("Invalid revision");
        return QTypeRevision();
    }
}

bool Moc::testFunctionRevision(FunctionDef *def)
{

    if (test(Q_REVISION_TOKEN)) {
        def->revision = parseRevision().toEncodedVersion<int>();
        return true;
    }

    return false;
}

// returns false if the function should be ignored
bool Moc::parseFunction(FunctionDef *def, bool inMacro)
{
    def->isVirtual = false;
    def->isStatic = false;
    //skip modifiers and attributes
    while (test(INLINE) || (test(STATIC) && (def->isStatic = true) == true) ||
        (test(VIRTUAL) && (def->isVirtual = true) == true) //mark as virtual
        || skipCxxAttributes() || testFunctionAttribute(def) || testFunctionRevision(def)) {}
    bool templateFunction = (lookup() == TEMPLATE);
    def->type = parseType();
    if (def->type.name.isEmpty()) {
        if (templateFunction)
            error("Template function as signal or slot");
        else
            error();
    }
    bool scopedFunctionName = false;
    if (test(LPAREN)) {
        def->name = def->type.name;
        scopedFunctionName = def->type.isScoped;
        def->type = Type("int");
    } else {
        Type tempType = parseType();;
        while (!tempType.name.isEmpty() && lookup() != LPAREN) {
            if (testFunctionAttribute(def->type.firstToken, def))
                ; // fine
            else if (def->type.firstToken == Q_SIGNALS_TOKEN)
                error();
            else if (def->type.firstToken == Q_SLOTS_TOKEN)
                error();
            else {
                if (!def->tag.isEmpty())
                    def->tag += ' ';
                def->tag += def->type.name;
            }
            def->type = tempType;
            tempType = parseType();
        }
        next(LPAREN, "Not a signal or slot declaration");
        def->name = tempType.name;
        scopedFunctionName = tempType.isScoped;
    }

    if (!test(RPAREN)) {
        parseFunctionArguments(def);
        next(RPAREN);
    }

    // support optional macros with compiler specific options
    while (test(IDENTIFIER))
        ;

    def->isConst = test(CONST);

    while (test(IDENTIFIER))
        ;

    if (inMacro) {
        next(RPAREN);
        prev();
    } else {
        if (test(THROW)) {
            next(LPAREN);
            until(RPAREN);
        }

        if (def->type.name == "auto" && test(ARROW))
            def->type = parseType(); // Parse trailing return-type

        if (test(SEMIC))
            ;
        else if ((def->inlineCode = test(LBRACE)))
            until(RBRACE);
        else if ((def->isAbstract = test(EQ)))
            until(SEMIC);
        else if (skipCxxAttributes())
            until(SEMIC);
        else
            error();
    }
    if (scopedFunctionName) {
        const QByteArray msg = "Function declaration " + def->name
                + " contains extra qualification. Ignoring as signal or slot.";
        warning(msg.constData());
        return false;
    }

    QList<QByteArray> typeNameParts = normalizeType(def->type.name).split(' ');
    if (typeNameParts.contains("auto")) {
        // We expected a trailing return type but we haven't seen one
        error("Function declared with auto as return type but missing trailing return type. "
              "Return type deduction is not supported.");
    }

    // we don't support references as return types, it's too dangerous
    if (def->type.referenceType == Type::Reference) {
        QByteArray rawName = def->type.rawName;
        def->type = Type("void");
        def->type.rawName = rawName;
    }

    def->normalizedType = normalizeType(def->type.name);
    return true;
}

// like parseFunction, but never aborts with an error
bool Moc::parseMaybeFunction(const ClassDef *cdef, FunctionDef *def)
{
    def->isVirtual = false;
    def->isStatic = false;
    //skip modifiers and attributes
    while (test(EXPLICIT) || test(INLINE) || (test(STATIC) && (def->isStatic = true) == true) ||
        (test(VIRTUAL) && (def->isVirtual = true) == true) //mark as virtual
        || skipCxxAttributes() || testFunctionAttribute(def) || testFunctionRevision(def)) {}
    bool tilde = test(TILDE);
    def->type = parseType();
    if (def->type.name.isEmpty())
        return false;
    bool scopedFunctionName = false;
    if (test(LPAREN)) {
        def->name = def->type.name;
        scopedFunctionName = def->type.isScoped;
        if (def->name == cdef->classname) {
            def->isDestructor = tilde;
            def->isConstructor = !tilde;
            def->type = Type();
        } else {
            def->type = Type("int");
        }
    } else {
        Type tempType = parseType();;
        while (!tempType.name.isEmpty() && lookup() != LPAREN) {
            if (testFunctionAttribute(def->type.firstToken, def))
                ; // fine
            else if (def->type.name == "Q_SIGNAL")
                def->isSignal = true;
            else if (def->type.name == "Q_SLOT")
                def->isSlot = true;
            else {
                if (!def->tag.isEmpty())
                    def->tag += ' ';
                def->tag += def->type.name;
            }
            def->type = tempType;
            tempType = parseType();
        }
        if (!test(LPAREN))
            return false;
        def->name = tempType.name;
        scopedFunctionName = tempType.isScoped;
    }

    // we don't support references as return types, it's too dangerous
    if (def->type.referenceType == Type::Reference) {
        QByteArray rawName = def->type.rawName;
        def->type = Type("void");
        def->type.rawName = rawName;
    }

    def->normalizedType = normalizeType(def->type.name);

    if (!test(RPAREN)) {
        parseFunctionArguments(def);
        if (!test(RPAREN))
            return false;
    }
    def->isConst = test(CONST);
    if (scopedFunctionName
        && (def->isSignal || def->isSlot || def->isInvokable)) {
        const QByteArray msg = "parsemaybe: Function declaration " + def->name
                + " contains extra qualification. Ignoring as signal or slot.";
        warning(msg.constData());
        return false;
    }
    return true;
}


void Moc::parse()
{
    QList<NamespaceDef> namespaceList;
    bool templateClass = false;
    while (hasNext()) {
        Token t = next();
        switch (t) {
            case NAMESPACE: {
                int rewind = index;
                if (test(IDENTIFIER)) {
                    QByteArray nsName = lexem();
                    QByteArrayList nested;
                    while (test(SCOPE)) {
                        next(IDENTIFIER);
                        nested.append(nsName);
                        nsName = lexem();
                    }
                    if (test(EQ)) {
                        // namespace Foo = Bar::Baz;
                        until(SEMIC);
                    } else if (test(LPAREN)) {
                        // Ignore invalid code such as: 'namespace __identifier("x")' (QTBUG-56634)
                        until(RPAREN);
                    } else if (!test(SEMIC)) {
                        NamespaceDef def;
                        def.classname = nsName;
                        def.doGenerate = currentFilenames.size() <= 1;

                        next(LBRACE);
                        def.begin = index - 1;
                        until(RBRACE);
                        def.end = index;
                        index = def.begin + 1;

                        for (int i = namespaceList.size() - 1; i >= 0; --i) {
                            if (inNamespace(&namespaceList.at(i))) {
                                def.qualified.prepend(namespaceList.at(i).classname + "::");
                            }
                        }
                        for (const QByteArray &ns : nested) {
                            NamespaceDef parentNs;
                            parentNs.classname = ns;
                            parentNs.qualified = def.qualified;
                            def.qualified += ns + "::";
                            parentNs.begin = def.begin;
                            parentNs.end = def.end;
                            namespaceList += parentNs;
                        }

                        while (inNamespace(&def) && hasNext()) {
                            switch (next()) {
                            case NAMESPACE:
                                if (test(IDENTIFIER)) {
                                    while (test(SCOPE))
                                        next(IDENTIFIER);
                                    if (test(EQ)) {
                                        // namespace Foo = Bar::Baz;
                                        until(SEMIC);
                                    } else if (!test(SEMIC)) {
                                        until(RBRACE);
                                    }
                                }
                                break;
                            case Q_NAMESPACE_TOKEN:
                                def.hasQNamespace = true;
                                break;
                            case Q_NAMESPACE_EXPORT_TOKEN:
                                next(LPAREN);
                                while (test(IDENTIFIER))
                                    {}
                                next(RPAREN);
                                def.hasQNamespace = true;
                                break;
                            case Q_ENUMS_TOKEN:
                            case Q_ENUM_NS_TOKEN:
                                parseEnumOrFlag(&def, false);
                                break;
                            case Q_ENUM_TOKEN:
                                error("Q_ENUM can't be used in a Q_NAMESPACE, use Q_ENUM_NS instead");
                                break;
                            case Q_FLAGS_TOKEN:
                            case Q_FLAG_NS_TOKEN:
                                parseEnumOrFlag(&def, true);
                                break;
                            case Q_FLAG_TOKEN:
                                error("Q_FLAG can't be used in a Q_NAMESPACE, use Q_FLAG_NS instead");
                                break;
                            case Q_DECLARE_FLAGS_TOKEN:
                                parseFlag(&def);
                                break;
                            case Q_CLASSINFO_TOKEN:
                                parseClassInfo(&def);
                                break;
                            case Q_MOC_INCLUDE_TOKEN:
                                // skip it, the namespace is parsed twice
                                next(LPAREN);
                                lexemUntil(RPAREN);
                                break;
                            case ENUM: {
                                EnumDef enumDef;
                                if (parseEnum(&enumDef))
                                    def.enumList += enumDef;
                            } break;
                            case CLASS:
                            case STRUCT: {
                                ClassDef classdef;
                                if (!parseClassHead(&classdef))
                                    continue;
                                while (inClass(&classdef) && hasNext())
                                    next(); // consume all Q_XXXX macros from this class
                            } break;
                            default: break;
                            }
                        }
                        namespaceList += def;
                        index = rewind;
                        if (!def.hasQNamespace && (!def.classInfoList.isEmpty() || !def.enumDeclarations.isEmpty()))
                            error("Namespace declaration lacks Q_NAMESPACE macro.");
                    }
                }
                break;
            }
            case SEMIC:
            case RBRACE:
                templateClass = false;
                break;
            case TEMPLATE:
                templateClass = true;
                break;
            case MOC_INCLUDE_BEGIN:
                currentFilenames.push(symbol().unquotedLexem());
                break;
            case MOC_INCLUDE_END:
                currentFilenames.pop();
                break;
            case Q_DECLARE_INTERFACE_TOKEN:
                parseDeclareInterface();
                break;
            case Q_DECLARE_METATYPE_TOKEN:
                parseDeclareMetatype();
                break;
            case Q_MOC_INCLUDE_TOKEN:
                parseMocInclude();
                break;
            case USING:
                if (test(NAMESPACE)) {
                    while (test(SCOPE) || test(IDENTIFIER))
                        ;
                    // Ignore invalid code such as: 'using namespace __identifier("x")' (QTBUG-63772)
                    if (test(LPAREN))
                        until(RPAREN);
                    next(SEMIC);
                }
                break;
            case CLASS:
            case STRUCT: {
                if (currentFilenames.size() <= 1)
                    break;

                ClassDef def;
                if (!parseClassHead(&def))
                    continue;

                while (inClass(&def) && hasNext()) {
                    switch (next()) {
                    case Q_OBJECT_TOKEN:
                        def.hasQObject = true;
                        break;
                    case Q_GADGET_TOKEN:
                        def.hasQGadget = true;
                        break;
                    default: break;
                    }
                }

                if (!def.hasQObject && !def.hasQGadget)
                    continue;

                for (int i = namespaceList.size() - 1; i >= 0; --i)
                    if (inNamespace(&namespaceList.at(i)))
                        def.qualified.prepend(namespaceList.at(i).classname + "::");

                QHash<QByteArray, QByteArray> &classHash = def.hasQObject ? knownQObjectClasses : knownGadgets;
                classHash.insert(def.classname, def.qualified);
                classHash.insert(def.qualified, def.qualified);

                continue; }
            default: break;
        }
        if ((t != CLASS && t != STRUCT)|| currentFilenames.size() > 1)
            continue;
        ClassDef def;
        if (parseClassHead(&def)) {
            FunctionDef::Access access = FunctionDef::Private;
            for (int i = namespaceList.size() - 1; i >= 0; --i)
                if (inNamespace(&namespaceList.at(i)))
                    def.qualified.prepend(namespaceList.at(i).classname + "::");
            while (inClass(&def) && hasNext()) {
                switch ((t = next())) {
                case PRIVATE:
                    access = FunctionDef::Private;
                    if (test(Q_SIGNALS_TOKEN))
                        error("Signals cannot have access specifier");
                    break;
                case PROTECTED:
                    access = FunctionDef::Protected;
                    if (test(Q_SIGNALS_TOKEN))
                        error("Signals cannot have access specifier");
                    break;
                case PUBLIC:
                    access = FunctionDef::Public;
                    if (test(Q_SIGNALS_TOKEN))
                        error("Signals cannot have access specifier");
                    break;
                case CLASS: {
                    ClassDef nestedDef;
                    if (parseClassHead(&nestedDef)) {
                        while (inClass(&nestedDef) && inClass(&def)) {
                            t = next();
                            if (t >= Q_META_TOKEN_BEGIN && t < Q_META_TOKEN_END)
                                error("Meta object features not supported for nested classes");
                        }
                    }
                } break;
                case Q_SIGNALS_TOKEN:
                    parseSignals(&def);
                    break;
                case Q_SLOTS_TOKEN:
                    switch (lookup(-1)) {
                    case PUBLIC:
                    case PROTECTED:
                    case PRIVATE:
                        parseSlots(&def, access);
                        break;
                    default:
                        error("Missing access specifier for slots");
                    }
                    break;
                case Q_OBJECT_TOKEN:
                    def.hasQObject = true;
                    if (templateClass)
                        error("Template classes not supported by Q_OBJECT");
                    if (def.classname != "Qt" && def.classname != "QObject" && def.superclassList.isEmpty())
                        error("Class contains Q_OBJECT macro but does not inherit from QObject");
                    break;
                case Q_GADGET_TOKEN:
                    def.hasQGadget = true;
                    if (templateClass)
                        error("Template classes not supported by Q_GADGET");
                    break;
                case Q_PROPERTY_TOKEN:
                    parseProperty(&def);
                    break;
                case Q_PLUGIN_METADATA_TOKEN:
                    parsePluginData(&def);
                    break;
                case Q_ENUMS_TOKEN:
                case Q_ENUM_TOKEN:
                    parseEnumOrFlag(&def, false);
                    break;
                case Q_ENUM_NS_TOKEN:
                    error("Q_ENUM_NS can't be used in a Q_OBJECT/Q_GADGET, use Q_ENUM instead");
                    break;
                case Q_FLAGS_TOKEN:
                case Q_FLAG_TOKEN:
                    parseEnumOrFlag(&def, true);
                    break;
                case Q_FLAG_NS_TOKEN:
                    error("Q_FLAG_NS can't be used in a Q_OBJECT/Q_GADGET, use Q_FLAG instead");
                    break;
                case Q_DECLARE_FLAGS_TOKEN:
                    parseFlag(&def);
                    break;
                case Q_CLASSINFO_TOKEN:
                    parseClassInfo(&def);
                    break;
                case Q_MOC_INCLUDE_TOKEN:
                    parseMocInclude();
                    break;
                case Q_INTERFACES_TOKEN:
                    parseInterfaces(&def);
                    break;
                case Q_PRIVATE_SLOT_TOKEN:
                    parseSlotInPrivate(&def, access);
                    break;
                case Q_PRIVATE_PROPERTY_TOKEN:
                    parsePrivateProperty(&def);
                    break;
                case ENUM: {
                    EnumDef enumDef;
                    if (parseEnum(&enumDef))
                        def.enumList += enumDef;
                } break;
                case SEMIC:
                case COLON:
                    break;
                default:
                    FunctionDef funcDef;
                    funcDef.access = access;
                    int rewind = index--;
                    if (parseMaybeFunction(&def, &funcDef)) {
                        if (funcDef.isConstructor) {
                            if ((access == FunctionDef::Public) && funcDef.isInvokable) {
                                def.constructorList += funcDef;
                                while (funcDef.arguments.size() > 0 && funcDef.arguments.constLast().isDefault) {
                                    funcDef.wasCloned = true;
                                    funcDef.arguments.removeLast();
                                    def.constructorList += funcDef;
                                }
                            }
                        } else if (funcDef.isDestructor) {
                            // don't care about destructors
                        } else {
                            if (access == FunctionDef::Public)
                                def.publicList += funcDef;
                            if (funcDef.isSlot) {
                                def.slotList += funcDef;
                                while (funcDef.arguments.size() > 0 && funcDef.arguments.constLast().isDefault) {
                                    funcDef.wasCloned = true;
                                    funcDef.arguments.removeLast();
                                    def.slotList += funcDef;
                                }
                                if (funcDef.revision > 0)
                                    ++def.revisionedMethods;
                            } else if (funcDef.isSignal) {
                                def.signalList += funcDef;
                                while (funcDef.arguments.size() > 0 && funcDef.arguments.constLast().isDefault) {
                                    funcDef.wasCloned = true;
                                    funcDef.arguments.removeLast();
                                    def.signalList += funcDef;
                                }
                                if (funcDef.revision > 0)
                                    ++def.revisionedMethods;
                            } else if (funcDef.isInvokable) {
                                def.methodList += funcDef;
                                while (funcDef.arguments.size() > 0 && funcDef.arguments.constLast().isDefault) {
                                    funcDef.wasCloned = true;
                                    funcDef.arguments.removeLast();
                                    def.methodList += funcDef;
                                }
                                if (funcDef.revision > 0)
                                    ++def.revisionedMethods;
                            }
                        }
                    } else {
                        index = rewind;
                    }
                }
            }

            next(RBRACE);

            if (!def.hasQObject && !def.hasQGadget && def.signalList.isEmpty() && def.slotList.isEmpty()
                && def.propertyList.isEmpty() && def.enumDeclarations.isEmpty())
                continue; // no meta object code required


            if (!def.hasQObject && !def.hasQGadget)
                error("Class declaration lacks Q_OBJECT macro.");

            // Add meta tags to the plugin meta data:
            if (!def.pluginData.iid.isEmpty())
                def.pluginData.metaArgs = metaArgs;

            checkSuperClasses(&def);
            checkProperties(&def);

            classList += def;
            QHash<QByteArray, QByteArray> &classHash = def.hasQObject ? knownQObjectClasses : knownGadgets;
            classHash.insert(def.classname, def.qualified);
            classHash.insert(def.qualified, def.qualified);
        }
    }
    for (const auto &n : qAsConst(namespaceList)) {
        if (!n.hasQNamespace)
            continue;
        ClassDef def;
        static_cast<BaseDef &>(def) = static_cast<BaseDef>(n);
        def.qualified += def.classname;
        def.hasQNamespace = true;
        auto it = std::find_if(classList.begin(), classList.end(), [&def](const ClassDef &val) {
            return def.classname == val.classname && def.qualified == val.qualified;
        });

        if (it != classList.end()) {
            it->classInfoList += def.classInfoList;
            it->enumDeclarations.insert(def.enumDeclarations);
            it->enumList += def.enumList;
            it->flagAliases.insert(def.flagAliases);
        } else {
            knownGadgets.insert(def.classname, def.qualified);
            knownGadgets.insert(def.qualified, def.qualified);
            if (n.doGenerate)
                classList += def;
        }
    }
}

static bool any_type_contains(const QList<PropertyDef> &properties, const QByteArray &pattern)
{
    for (const auto &p : properties) {
        if (p.type.contains(pattern))
            return true;
    }
    return false;
}

static bool any_arg_contains(const QList<FunctionDef> &functions, const QByteArray &pattern)
{
    for (const auto &f : functions) {
        for (const auto &arg : f.arguments) {
            if (arg.normalizedType.contains(pattern))
                return true;
        }
    }
    return false;
}

static QByteArrayList make_candidates()
{
    QByteArrayList result;
    result
#define STREAM_SMART_POINTER(SMART_POINTER) << #SMART_POINTER
        QT_FOR_EACH_AUTOMATIC_TEMPLATE_SMART_POINTER(STREAM_SMART_POINTER)
#undef STREAM_SMART_POINTER
#define STREAM_1ARG_TEMPLATE(TEMPLATENAME) << #TEMPLATENAME
        QT_FOR_EACH_AUTOMATIC_TEMPLATE_1ARG(STREAM_1ARG_TEMPLATE)
#undef STREAM_1ARG_TEMPLATE
        ;
    return result;
}

static QByteArrayList requiredQtContainers(const QList<ClassDef> &classes)
{
    static const QByteArrayList candidates = make_candidates();

    QByteArrayList required;
    required.reserve(candidates.size());

    bool needsQProperty = false;

    for (const auto &candidate : candidates) {
        const QByteArray pattern = candidate + '<';

        for (const auto &c : classes) {
            for (const auto &p : c.propertyList)
                needsQProperty |= !p.bind.isEmpty();
            if (any_type_contains(c.propertyList, pattern) ||
                    any_arg_contains(c.slotList, pattern) ||
                    any_arg_contains(c.signalList, pattern) ||
                    any_arg_contains(c.methodList, pattern)) {
                required.push_back(candidate);
                break;
            }
        }
    }

    if (needsQProperty)
        required.push_back("QProperty");

    return required;
}

void Moc::generate(FILE *out, FILE *jsonOutput)
{
    QByteArray fn = filename;
    int i = filename.length()-1;
    while (i > 0 && filename.at(i - 1) != '/' && filename.at(i - 1) != '\\')
        --i;                                // skip path
    if (i >= 0)
        fn = filename.mid(i);
    fprintf(out, "/****************************************************************************\n"
            "** Meta object code from reading C++ file '%s'\n**\n" , fn.constData());
    fprintf(out, "** Created by: The Qt Meta Object Compiler version %d (Qt %s)\n**\n" , mocOutputRevision, QT_VERSION_STR);
    fprintf(out, "** WARNING! All changes made in this file will be lost!\n"
            "*****************************************************************************/\n\n");

    fprintf(out, "#include <memory>\n");  // For std::addressof
    if (!noInclude) {
        if (includePath.size() && !includePath.endsWith('/'))
            includePath += '/';
        for (int i = 0; i < includeFiles.size(); ++i) {
            QByteArray inc = includeFiles.at(i);
            if (inc.at(0) != '<' && inc.at(0) != '"') {
                if (includePath.size() && includePath != "./")
                    inc.prepend(includePath);
                inc = '\"' + inc + '\"';
            }
            fprintf(out, "#include %s\n", inc.constData());
        }
    }
    if (classList.size() && classList.constFirst().classname == "Qt")
        fprintf(out, "#include <QtCore/qobject.h>\n");

    fprintf(out, "#include <QtCore/qbytearray.h>\n"); // For QByteArrayData
    fprintf(out, "#include <QtCore/qmetatype.h>\n");  // For QMetaType::Type
    if (mustIncludeQPluginH)
        fprintf(out, "#include <QtCore/qplugin.h>\n");

    const auto qtContainers = requiredQtContainers(classList);
    for (const QByteArray &qtContainer : qtContainers)
        fprintf(out, "#include <QtCore/%s>\n", qtContainer.constData());


    fprintf(out, "#if !defined(Q_MOC_OUTPUT_REVISION)\n"
            "#error \"The header file '%s' doesn't include <QObject>.\"\n", fn.constData());
    fprintf(out, "#elif Q_MOC_OUTPUT_REVISION != %d\n", mocOutputRevision);
    fprintf(out, "#error \"This file was generated using the moc from %s."
            " It\"\n#error \"cannot be used with the include files from"
            " this version of Qt.\"\n#error \"(The moc has changed too"
            " much.)\"\n", QT_VERSION_STR);
    fprintf(out, "#endif\n\n");

    fprintf(out, "QT_BEGIN_MOC_NAMESPACE\n");
    fprintf(out, "QT_WARNING_PUSH\n");
    fprintf(out, "QT_WARNING_DISABLE_DEPRECATED\n");

    fputs("", out);
    for (i = 0; i < classList.size(); ++i) {
        Generator generator(&classList[i], metaTypes, knownQObjectClasses, knownGadgets, out, requireCompleteTypes);
        generator.generateCode();
    }
    fputs("", out);

    fprintf(out, "QT_WARNING_POP\n");
    fprintf(out, "QT_END_MOC_NAMESPACE\n");

    if (jsonOutput) {
        QJsonObject mocData;
        mocData[QLatin1String("outputRevision")] = mocOutputRevision;
        mocData[QLatin1String("inputFile")] = QLatin1String(fn.constData());

        QJsonArray classesJsonFormatted;

        for (const ClassDef &cdef: qAsConst(classList))
            classesJsonFormatted.append(cdef.toJson());

        if (!classesJsonFormatted.isEmpty())
            mocData[QLatin1String("classes")] = classesJsonFormatted;

        QJsonDocument jsonDoc(mocData);
        fputs(jsonDoc.toJson().constData(), jsonOutput);
    }
}

void Moc::parseSlots(ClassDef *def, FunctionDef::Access access)
{
    QTypeRevision defaultRevision;
    if (test(Q_REVISION_TOKEN))
        defaultRevision = parseRevision();

    next(COLON);
    while (inClass(def) && hasNext()) {
        switch (next()) {
        case PUBLIC:
        case PROTECTED:
        case PRIVATE:
        case Q_SIGNALS_TOKEN:
        case Q_SLOTS_TOKEN:
            prev();
            return;
        case SEMIC:
            continue;
        case FRIEND:
            until(SEMIC);
            continue;
        case USING:
            error("'using' directive not supported in 'slots' section");
        default:
            prev();
        }

        FunctionDef funcDef;
        funcDef.access = access;
        if (!parseFunction(&funcDef))
            continue;
        if (funcDef.revision > 0) {
            ++def->revisionedMethods;
        } else if (defaultRevision.isValid()) {
            funcDef.revision = defaultRevision.toEncodedVersion<int>();
            ++def->revisionedMethods;
        }
        def->slotList += funcDef;
        while (funcDef.arguments.size() > 0 && funcDef.arguments.constLast().isDefault) {
            funcDef.wasCloned = true;
            funcDef.arguments.removeLast();
            def->slotList += funcDef;
        }
    }
}

void Moc::parseSignals(ClassDef *def)
{
    QTypeRevision defaultRevision;
    if (test(Q_REVISION_TOKEN))
        defaultRevision = parseRevision();

    next(COLON);
    while (inClass(def) && hasNext()) {
        switch (next()) {
        case PUBLIC:
        case PROTECTED:
        case PRIVATE:
        case Q_SIGNALS_TOKEN:
        case Q_SLOTS_TOKEN:
            prev();
            return;
        case SEMIC:
            continue;
        case FRIEND:
            until(SEMIC);
            continue;
        case USING:
            error("'using' directive not supported in 'signals' section");
        default:
            prev();
        }
        FunctionDef funcDef;
        funcDef.access = FunctionDef::Public;
        parseFunction(&funcDef);
        if (funcDef.isVirtual)
            warning("Signals cannot be declared virtual");
        if (funcDef.inlineCode)
            error("Not a signal declaration");
        if (funcDef.revision > 0) {
            ++def->revisionedMethods;
        } else if (defaultRevision.isValid()) {
            funcDef.revision = defaultRevision.toEncodedVersion<int>();
            ++def->revisionedMethods;
        }
        def->signalList += funcDef;
        while (funcDef.arguments.size() > 0 && funcDef.arguments.constLast().isDefault) {
            funcDef.wasCloned = true;
            funcDef.arguments.removeLast();
            def->signalList += funcDef;
        }
    }
}

void Moc::createPropertyDef(PropertyDef &propDef)
{
    propDef.location = index;

    QByteArray type = parseType().name;
    if (type.isEmpty())
        error();
    propDef.designable = propDef.scriptable = propDef.stored = "true";
    propDef.user = "false";
    /*
      The Q_PROPERTY construct cannot contain any commas, since
      commas separate macro arguments. We therefore expect users
      to type "QMap" instead of "QMap<QString, QVariant>". For
      coherence, we also expect the same for
      QValueList<QVariant>, the other template class supported by
      QVariant.
    */
    type = normalizeType(type);
    if (type == "QMap")
        type = "QMap<QString,QVariant>";
    else if (type == "QValueList")
        type = "QValueList<QVariant>";
    else if (type == "LongLong")
        type = "qlonglong";
    else if (type == "ULongLong")
        type = "qulonglong";

    propDef.type = type;

    next();
    propDef.name = lexem();

    parsePropertyAttributes(propDef);
}

void Moc::parsePropertyAttributes(PropertyDef &propDef)
{
    auto checkIsFunction = [&](const QByteArray &def, const char *name) {
        if (def.endsWith(')')) {
            QByteArray msg = "Providing a function for ";
            msg += name;
            msg += " in a property declaration is not be supported in Qt 6.";
            error(msg.constData());
        }
    };

    while (test(IDENTIFIER)) {
        const QByteArray l = lexem();
        if (l[0] == 'C' && l == "CONSTANT") {
            propDef.constant = true;
            continue;
        } else if (l[0] == 'F' && l == "FINAL") {
            propDef.final = true;
            continue;
        } else if (l[0] == 'N' && l == "NAME") {
            next(IDENTIFIER);
            propDef.name = lexem();
            continue;
        } else if (l[0] == 'R' && l == "REQUIRED") {
            propDef.required = true;
            continue;
        } else if (l[0] == 'R' && l == "REVISION" && test(LPAREN)) {
            prev();
            propDef.revision = parseRevision().toEncodedVersion<int>();
            continue;
        }

        QByteArray v, v2;
        if (test(LPAREN)) {
            v = lexemUntil(RPAREN);
            v = v.mid(1, v.length() - 2); // removes the '(' and ')'
        } else if (test(INTEGER_LITERAL)) {
            v = lexem();
            if (l != "REVISION")
                error(1);
        } else {
            next(IDENTIFIER);
            v = lexem();
            if (test(LPAREN))
                v2 = lexemUntil(RPAREN);
            else if (v != "true" && v != "false")
                v2 = "()";
        }
        switch (l[0]) {
        case 'M':
            if (l == "MEMBER")
                propDef.member = v;
            else
                error(2);
            break;
        case 'R':
            if (l == "READ")
                propDef.read = v;
            else if (l == "RESET")
                propDef.reset = v + v2;
            else if (l == "REVISION") {
                bool ok = false;
                const int minor = v.toInt(&ok);
                if (!ok || !QTypeRevision::isValidSegment(minor))
                    error(1);
                propDef.revision = QTypeRevision::fromMinorVersion(minor).toEncodedVersion<int>();
            } else
                error(2);
            break;
        case 'S':
            if (l == "SCRIPTABLE") {
                propDef.scriptable = v + v2;
                checkIsFunction(propDef.scriptable, "SCRIPTABLE");
            } else if (l == "STORED") {
                propDef.stored = v + v2;
                checkIsFunction(propDef.stored, "STORED");
            } else
                error(2);
            break;
        case 'W': if (l != "WRITE") error(2);
            propDef.write = v;
            break;
        case 'B': if (l != "BINDABLE") error(2);
            propDef.bind = v;
            break;
        case 'D': if (l != "DESIGNABLE") error(2);
            propDef.designable = v + v2;
            checkIsFunction(propDef.designable, "DESIGNABLE");
            break;
        case 'N': if (l != "NOTIFY") error(2);
            propDef.notify = v;
            break;
        case 'U': if (l != "USER") error(2);
            propDef.user = v + v2;
            checkIsFunction(propDef.user, "USER");
            break;
        default:
            error(2);
        }
    }
    if (propDef.constant && !propDef.write.isNull()) {
        const QByteArray msg = "Property declaration " + propDef.name
                + " is both WRITEable and CONSTANT. CONSTANT will be ignored.";
        propDef.constant = false;
        warning(msg.constData());
    }
    if (propDef.constant && !propDef.notify.isNull()) {
        const QByteArray msg = "Property declaration " + propDef.name
                + " is both NOTIFYable and CONSTANT. CONSTANT will be ignored.";
        propDef.constant = false;
        warning(msg.constData());
    }
    if (propDef.constant && !propDef.bind.isNull()) {
        const QByteArray msg = "Property declaration " + propDef.name
                + " is both BINDable and CONSTANT. CONSTANT will be ignored.";
        propDef.constant = false;
        warning(msg.constData());
    }
}

void Moc::parseProperty(ClassDef *def)
{
    next(LPAREN);
    PropertyDef propDef;
    createPropertyDef(propDef);
    next(RPAREN);

    def->propertyList += propDef;
}

void Moc::parsePluginData(ClassDef *def)
{
    next(LPAREN);
    QByteArray metaData;
    while (test(IDENTIFIER)) {
        QByteArray l = lexem();
        if (l == "IID") {
            next(STRING_LITERAL);
            def->pluginData.iid = unquotedLexem();
        } else if (l == "URI") {
            next(STRING_LITERAL);
            def->pluginData.uri = unquotedLexem();
        } else if (l == "FILE") {
            next(STRING_LITERAL);
            QByteArray metaDataFile = unquotedLexem();
            QFileInfo fi(QFileInfo(QString::fromLocal8Bit(currentFilenames.top().constData())).dir(), QString::fromLocal8Bit(metaDataFile.constData()));
            for (int j = 0; j < includes.size() && !fi.exists(); ++j) {
                const IncludePath &p = includes.at(j);
                if (p.isFrameworkPath)
                    continue;

                fi.setFile(QString::fromLocal8Bit(p.path.constData()), QString::fromLocal8Bit(metaDataFile.constData()));
                // try again, maybe there's a file later in the include paths with the same name
                if (fi.isDir()) {
                    fi = QFileInfo();
                    continue;
                }
            }
            if (!fi.exists()) {
                const QByteArray msg = "Plugin Metadata file " + lexem()
                        + " does not exist. Declaration will be ignored";
                error(msg.constData());
                return;
            }
            QFile file(fi.canonicalFilePath());
            if (!file.open(QFile::ReadOnly)) {
                QByteArray msg = "Plugin Metadata file " + lexem() + " could not be opened: "
                    + file.errorString().toUtf8();
                error(msg.constData());
                return;
            }
            parsedPluginMetadataFiles.append(fi.canonicalFilePath());
            metaData = file.readAll();
        }
    }

    if (!metaData.isEmpty()) {
        def->pluginData.metaData = QJsonDocument::fromJson(metaData);
        if (!def->pluginData.metaData.isObject()) {
            const QByteArray msg = "Plugin Metadata file " + lexem()
                    + " does not contain a valid JSON object. Declaration will be ignored";
            warning(msg.constData());
            def->pluginData.iid = QByteArray();
            def->pluginData.uri = QByteArray();
            return;
        }
    }

    mustIncludeQPluginH = true;
    next(RPAREN);
}

QByteArray Moc::parsePropertyAccessor()
{
    int nesting = 0;
    QByteArray accessor;
    while (1) {
        Token t = peek();
        if (!nesting && (t == RPAREN || t == COMMA))
            break;
        t = next();
        if (t == LPAREN)
            ++nesting;
        if (t == RPAREN)
            --nesting;
        accessor += lexem();
    }
    return accessor;
}

void Moc::parsePrivateProperty(ClassDef *def)
{
    next(LPAREN);
    PropertyDef propDef;
    propDef.inPrivateClass = parsePropertyAccessor();

    next(COMMA);

    createPropertyDef(propDef);

    def->propertyList += propDef;
}

void Moc::parseEnumOrFlag(BaseDef *def, bool isFlag)
{
    next(LPAREN);
    QByteArray identifier;
    while (test(IDENTIFIER)) {
        identifier = lexem();
        while (test(SCOPE) && test(IDENTIFIER)) {
            identifier += "::";
            identifier += lexem();
        }
        def->enumDeclarations[identifier] = isFlag;
    }
    next(RPAREN);
}

void Moc::parseFlag(BaseDef *def)
{
    next(LPAREN);
    QByteArray flagName, enumName;
    while (test(IDENTIFIER)) {
        flagName = lexem();
        while (test(SCOPE) && test(IDENTIFIER)) {
            flagName += "::";
            flagName += lexem();
        }
    }
    next(COMMA);
    while (test(IDENTIFIER)) {
        enumName = lexem();
        while (test(SCOPE) && test(IDENTIFIER)) {
            enumName += "::";
            enumName += lexem();
        }
    }

    def->flagAliases.insert(enumName, flagName);
    next(RPAREN);
}

Moc::EncounteredQmlMacro Moc::parseClassInfo(BaseDef *def)
{
    bool encounteredQmlMacro = false;
    next(LPAREN);
    ClassInfoDef infoDef;
    next(STRING_LITERAL);
    infoDef.name = symbol().unquotedLexem();
    if (infoDef.name.startsWith("QML."))
        encounteredQmlMacro = true;
    next(COMMA);
    if (test(STRING_LITERAL)) {
        infoDef.value = symbol().unquotedLexem();
    } else if (test(Q_REVISION_TOKEN)) {
        infoDef.value = QByteArray::number(parseRevision().toEncodedVersion<quint16>());
    } else {
        // support Q_CLASSINFO("help", QT_TR_NOOP("blah"))
        next(IDENTIFIER);
        next(LPAREN);
        next(STRING_LITERAL);
        infoDef.value = symbol().unquotedLexem();
        next(RPAREN);
    }
    next(RPAREN);
    def->classInfoList += infoDef;
    return encounteredQmlMacro ? EncounteredQmlMacro::Yes : EncounteredQmlMacro::No;
}

void Moc::parseClassInfo(ClassDef *def)
{
    if (parseClassInfo(static_cast<BaseDef *>(def)) == EncounteredQmlMacro::Yes)
        def->requireCompleteMethodTypes = true;
}

void Moc::parseInterfaces(ClassDef *def)
{
    next(LPAREN);
    while (test(IDENTIFIER)) {
        QList<ClassDef::Interface> iface;
        iface += ClassDef::Interface(lexem());
        while (test(SCOPE)) {
            iface.last().className += lexem();
            next(IDENTIFIER);
            iface.last().className += lexem();
        }
        while (test(COLON)) {
            next(IDENTIFIER);
            iface += ClassDef::Interface(lexem());
            while (test(SCOPE)) {
                iface.last().className += lexem();
                next(IDENTIFIER);
                iface.last().className += lexem();
            }
        }
        // resolve from classnames to interface ids
        for (int i = 0; i < iface.count(); ++i) {
            const QByteArray iid = interface2IdMap.value(iface.at(i).className);
            if (iid.isEmpty())
                error("Undefined interface");

            iface[i].interfaceId = iid;
        }
        def->interfaceList += iface;
    }
    next(RPAREN);
}

void Moc::parseDeclareInterface()
{
    next(LPAREN);
    QByteArray interface;
    next(IDENTIFIER);
    interface += lexem();
    while (test(SCOPE)) {
        interface += lexem();
        next(IDENTIFIER);
        interface += lexem();
    }
    next(COMMA);
    QByteArray iid;
    if (test(STRING_LITERAL)) {
        iid = lexem();
    } else {
        next(IDENTIFIER);
        iid = lexem();
    }
    interface2IdMap.insert(interface, iid);
    next(RPAREN);
}

void Moc::parseDeclareMetatype()
{
    next(LPAREN);
    QByteArray typeName = lexemUntil(RPAREN);
    typeName.remove(0, 1);
    typeName.chop(1);
    metaTypes.append(typeName);
}

void Moc::parseMocInclude()
{
    next(LPAREN);
    QByteArray include = lexemUntil(RPAREN);
    // remove parentheses
    include.remove(0, 1);
    include.chop(1);
    includeFiles.append(include);
}

void Moc::parseSlotInPrivate(ClassDef *def, FunctionDef::Access access)
{
    next(LPAREN);
    FunctionDef funcDef;
    next(IDENTIFIER);
    funcDef.inPrivateClass = lexem();
    // also allow void functions
    if (test(LPAREN)) {
        next(RPAREN);
        funcDef.inPrivateClass += "()";
    }
    next(COMMA);
    funcDef.access = access;
    parseFunction(&funcDef, true);
    def->slotList += funcDef;
    while (funcDef.arguments.size() > 0 && funcDef.arguments.constLast().isDefault) {
        funcDef.wasCloned = true;
        funcDef.arguments.removeLast();
        def->slotList += funcDef;
    }
    if (funcDef.revision > 0)
        ++def->revisionedMethods;

}

QByteArray Moc::lexemUntil(Token target)
{
    int from = index;
    until(target);
    QByteArray s;
    while (from <= index) {
        QByteArray n = symbols.at(from++-1).lexem();
        if (s.size() && n.size()) {
            char prev = s.at(s.size()-1);
            char next = n.at(0);
            if ((is_ident_char(prev) && is_ident_char(next))
                || (prev == '<' && next == ':')
                || (prev == '>' && next == '>'))
                s += ' ';
        }
        s += n;
    }
    return s;
}

bool Moc::until(Token target) {
    int braceCount = 0;
    int brackCount = 0;
    int parenCount = 0;
    int angleCount = 0;
    if (index) {
        switch(symbols.at(index-1).token) {
        case LBRACE: ++braceCount; break;
        case LBRACK: ++brackCount; break;
        case LPAREN: ++parenCount; break;
        case LANGLE: ++angleCount; break;
        default: break;
        }
    }

    //when searching commas within the default argument, we should take care of template depth (anglecount)
    // unfortunatelly, we do not have enough semantic information to know if '<' is the operator< or
    // the beginning of a template type. so we just use heuristics.
    int possible = -1;

    while (index < symbols.size()) {
        Token t = symbols.at(index++).token;
        switch (t) {
        case LBRACE: ++braceCount; break;
        case RBRACE: --braceCount; break;
        case LBRACK: ++brackCount; break;
        case RBRACK: --brackCount; break;
        case LPAREN: ++parenCount; break;
        case RPAREN: --parenCount; break;
        case LANGLE:
            if (parenCount == 0 && braceCount == 0)
                ++angleCount;
          break;
        case RANGLE:
            if (parenCount == 0 && braceCount == 0)
                --angleCount;
          break;
        case GTGT:
            if (parenCount == 0 && braceCount == 0) {
                angleCount -= 2;
                t = RANGLE;
            }
            break;
        default: break;
        }
        if (t == target
            && braceCount <= 0
            && brackCount <= 0
            && parenCount <= 0
            && (target != RANGLE || angleCount <= 0)) {
            if (target != COMMA || angleCount <= 0)
                return true;
            possible = index;
        }

        if (target == COMMA && t == EQ && possible != -1) {
            index = possible;
            return true;
        }

        if (braceCount < 0 || brackCount < 0 || parenCount < 0
            || (target == RANGLE && angleCount < 0)) {
            --index;
            break;
        }

        if (braceCount <= 0 && t == SEMIC) {
            // Abort on semicolon. Allow recovering bad template parsing (QTBUG-31218)
            break;
        }
    }

    if (target == COMMA && angleCount != 0 && possible != -1) {
        index = possible;
        return true;
    }

    return false;
}

void Moc::checkSuperClasses(ClassDef *def)
{
    const QByteArray firstSuperclass = def->superclassList.value(0).first;

    if (!knownQObjectClasses.contains(firstSuperclass)) {
        // enable once we /require/ include paths
#if 0
        const QByteArray msg
                = "Class "
                + def->className
                + " contains the Q_OBJECT macro and inherits from "
                + def->superclassList.value(0)
                + " but that is not a known QObject subclass. You may get compilation errors.";
        warning(msg.constData());
#endif
        return;
    }
    for (int i = 1; i < def->superclassList.count(); ++i) {
        const QByteArray superClass = def->superclassList.at(i).first;
        if (knownQObjectClasses.contains(superClass)) {
            const QByteArray msg
                    = "Class "
                    + def->classname
                    + " inherits from two QObject subclasses "
                    + firstSuperclass
                    + " and "
                    + superClass
                    + ". This is not supported!";
            warning(msg.constData());
        }

        if (interface2IdMap.contains(superClass)) {
            bool registeredInterface = false;
            for (int i = 0; i < def->interfaceList.count(); ++i)
                if (def->interfaceList.at(i).constFirst().className == superClass) {
                    registeredInterface = true;
                    break;
                }

            if (!registeredInterface) {
                const QByteArray msg
                        = "Class "
                        + def->classname
                        + " implements the interface "
                        + superClass
                        + " but does not list it in Q_INTERFACES. qobject_cast to "
                        + superClass
                        + " will not work!";
                warning(msg.constData());
            }
        }
    }
}

void Moc::checkProperties(ClassDef *cdef)
{
    //
    // specify get function, for compatibiliy we accept functions
    // returning pointers, or const char * for QByteArray.
    //
    QDuplicateTracker<QByteArray> definedProperties;
    for (int i = 0; i < cdef->propertyList.count(); ++i) {
        PropertyDef &p = cdef->propertyList[i];
        if (definedProperties.hasSeen(p.name)) {
            QByteArray msg = "The property '" + p.name + "' is defined multiple times in class " + cdef->classname + ".";
            warning(msg.constData());
        }

        if (p.read.isEmpty() && p.member.isEmpty() && p.bind.isEmpty()) {
            const int rewind = index;
            if (p.location >= 0)
                index = p.location;
            QByteArray msg = "Property declaration " + p.name + " has neither an associated QProperty<> member"
                             ", nor a READ accessor function nor an associated MEMBER variable. The property will be invalid.";
            warning(msg.constData());
            index = rewind;
            if (p.write.isEmpty()) {
                cdef->propertyList.removeAt(i);
                --i;
            }
            continue;
        }

        for (int j = 0; j < cdef->publicList.count(); ++j) {
            const FunctionDef &f = cdef->publicList.at(j);
            if (f.name != p.read)
                continue;
            if (!f.isConst) // get  functions must be const
                continue;
            if (f.arguments.size()) // and must not take any arguments
                continue;
            PropertyDef::Specification spec = PropertyDef::ValueSpec;
            QByteArray tmp = f.normalizedType;
            if (p.type == "QByteArray" && tmp == "const char *")
                tmp = "QByteArray";
            if (tmp.left(6) == "const ")
                tmp = tmp.mid(6);
            if (p.type != tmp && tmp.endsWith('*')) {
                tmp.chop(1);
                spec = PropertyDef::PointerSpec;
            } else if (f.type.name.endsWith('&')) { // raw type, not normalized type
                spec = PropertyDef::ReferenceSpec;
            }
            if (p.type != tmp)
                continue;
            p.gspec = spec;
            break;
        }
        if (!p.notify.isEmpty()) {
            int notifyId = -1;
            for (int j = 0; j < cdef->signalList.count(); ++j) {
                const FunctionDef &f = cdef->signalList.at(j);
                if (f.name != p.notify) {
                    continue;
                } else {
                    notifyId = j /* Signal indexes start from 0 */;
                    break;
                }
            }
            p.notifyId = notifyId;
            if (notifyId == -1) {
                int index = cdef->nonClassSignalList.indexOf(p.notify);
                if (index == -1) {
                    cdef->nonClassSignalList << p.notify;
                    p.notifyId = -1 - cdef->nonClassSignalList.count();
                } else {
                    p.notifyId = -2 - index;
                }
            }
        }
    }
}

QJsonObject ClassDef::toJson() const
{
    QJsonObject cls;
    cls[QLatin1String("className")] = QString::fromUtf8(classname.constData());
    cls[QLatin1String("qualifiedClassName")] = QString::fromUtf8(qualified.constData());

    QJsonArray classInfos;
    for (const auto &info: qAsConst(classInfoList)) {
        QJsonObject infoJson;
        infoJson[QLatin1String("name")] = QString::fromUtf8(info.name);
        infoJson[QLatin1String("value")] = QString::fromUtf8(info.value);
        classInfos.append(infoJson);
    }

    if (classInfos.size())
        cls[QLatin1String("classInfos")] = classInfos;

    const auto appendFunctions = [&cls](const QString &type, const QList<FunctionDef> &funcs) {
        QJsonArray jsonFuncs;

        for (const FunctionDef &fdef: funcs)
            jsonFuncs.append(fdef.toJson());

        if (!jsonFuncs.isEmpty())
            cls[type] = jsonFuncs;
    };

    appendFunctions(QLatin1String("signals"), signalList);
    appendFunctions(QLatin1String("slots"), slotList);
    appendFunctions(QLatin1String("constructors"), constructorList);
    appendFunctions(QLatin1String("methods"), methodList);

    QJsonArray props;

    for (const PropertyDef &propDef: qAsConst(propertyList))
        props.append(propDef.toJson());

    if (!props.isEmpty())
        cls[QLatin1String("properties")] = props;

    if (hasQObject)
        cls[QLatin1String("object")] = true;
    if (hasQGadget)
        cls[QLatin1String("gadget")] = true;
    if (hasQNamespace)
        cls[QLatin1String("namespace")] = true;

    QJsonArray superClasses;

    for (const auto &super: qAsConst(superclassList)) {
        const auto name = super.first;
        const auto access = super.second;
        QJsonObject superCls;
        superCls[QLatin1String("name")] = QString::fromUtf8(name);
        FunctionDef::accessToJson(&superCls, access);
        superClasses.append(superCls);
    }

    if (!superClasses.isEmpty())
        cls[QLatin1String("superClasses")] = superClasses;

    QJsonArray enums;
    for (const EnumDef &enumDef: qAsConst(enumList))
        enums.append(enumDef.toJson(*this));
    if (!enums.isEmpty())
        cls[QLatin1String("enums")] = enums;

    QJsonArray ifaces;
    for (const QList<Interface> &ifaceList : interfaceList) {
        QJsonArray jsonList;
        for (const Interface &iface: ifaceList) {
            QJsonObject ifaceJson;
            ifaceJson[QLatin1String("id")] = QString::fromUtf8(iface.interfaceId);
            ifaceJson[QLatin1String("className")] = QString::fromUtf8(iface.className);
            jsonList.append(ifaceJson);
        }
        ifaces.append(jsonList);
    }
    if (!ifaces.isEmpty())
        cls[QLatin1String("interfaces")] = ifaces;

    return cls;
}

QJsonObject FunctionDef::toJson() const
{
    QJsonObject fdef;
    fdef[QLatin1String("name")] = QString::fromUtf8(name);
    if (!tag.isEmpty())
        fdef[QLatin1String("tag")] = QString::fromUtf8(tag);
    fdef[QLatin1String("returnType")] = QString::fromUtf8(normalizedType);

    QJsonArray args;
    for (const ArgumentDef &arg: arguments)
        args.append(arg.toJson());

    if (!args.isEmpty())
        fdef[QLatin1String("arguments")] = args;

    accessToJson(&fdef, access);

    if (revision > 0)
        fdef[QLatin1String("revision")] = revision;

    return fdef;
}

void FunctionDef::accessToJson(QJsonObject *obj, FunctionDef::Access acs)
{
    switch (acs) {
    case Private: (*obj)[QLatin1String("access")] = QLatin1String("private"); break;
    case Public: (*obj)[QLatin1String("access")] = QLatin1String("public"); break;
    case Protected: (*obj)[QLatin1String("access")] = QLatin1String("protected"); break;
    }
}

QJsonObject ArgumentDef::toJson() const
{
    QJsonObject arg;
    arg[QLatin1String("type")] = QString::fromUtf8(normalizedType);
    if (!name.isEmpty())
        arg[QLatin1String("name")] = QString::fromUtf8(name);
    return arg;
}

QJsonObject PropertyDef::toJson() const
{
    QJsonObject prop;
    prop[QLatin1String("name")] = QString::fromUtf8(name);
    prop[QLatin1String("type")] = QString::fromUtf8(type);

    const auto jsonify = [&prop](const char *str, const QByteArray &member) {
        if (!member.isEmpty())
            prop[QLatin1String(str)] = QString::fromUtf8(member);
    };

    jsonify("member", member);
    jsonify("read", read);
    jsonify("write", write);
    jsonify("bindable", bind);
    jsonify("reset", reset);
    jsonify("notify", notify);
    jsonify("privateClass", inPrivateClass);

    const auto jsonifyBoolOrString = [&prop](const char *str, const QByteArray &boolOrString) {
        QJsonValue value;
        if (boolOrString == "true")
            value = true;
        else if (boolOrString == "false")
            value = false;
        else
            value = QString::fromUtf8(boolOrString); // function name to query at run-time
        prop[QLatin1String(str)] = value;
    };

    jsonifyBoolOrString("designable", designable);
    jsonifyBoolOrString("scriptable", scriptable);
    jsonifyBoolOrString("stored", stored);
    jsonifyBoolOrString("user", user);

    prop[QLatin1String("constant")] = constant;
    prop[QLatin1String("final")] = final;
    prop[QLatin1String("required")] = required;

    if (revision > 0)
        prop[QLatin1String("revision")] = revision;

    return prop;
}

QJsonObject EnumDef::toJson(const ClassDef &cdef) const
{
    QJsonObject def;
    def[QLatin1String("name")] = QString::fromUtf8(name);
    if (!enumName.isEmpty())
        def[QLatin1String("alias")] = QString::fromUtf8(enumName);
    def[QLatin1String("isFlag")] = cdef.enumDeclarations.value(name);
    def[QLatin1String("isClass")] = isEnumClass;

    QJsonArray valueArr;
    for (const QByteArray &value: values)
        valueArr.append(QString::fromUtf8(value));
    if (!valueArr.isEmpty())
        def[QLatin1String("values")] = valueArr;

    return def;
}

QT_END_NAMESPACE
