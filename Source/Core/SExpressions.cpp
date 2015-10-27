#include "Core/Core.h"
#include "Core/SExpressions.h"
#include "Core/Floats.h"
#include <initializer_list>
#include <climits>
#include <cerrno>
#include <cstdlib>

namespace SExp
{
	struct FatalParseException
	{
		Core::TextFileLocus locus;
		std::string message;
		FatalParseException(const Core::TextFileLocus& inLocus,std::string&& inMessage)
		: locus(inLocus), message(std::move(inMessage)) {}
	};

	struct StreamState
	{
		const char* next;
		Core::TextFileLocus locus;

		StreamState(const char* inString)
		:	next(inString)
		{}
		
		char peek() const
		{
			return *next;
		}
		char consume()
		{
			auto result = *next;
			advance();
			return result;
		}
		Core::TextFileLocus getLocus() const { return locus; }
		
		void advance()
		{
			switch(*next)
			{
			case 0: throw new FatalParseException(locus,"unexpected end of file");
			case '\n': locus.newlines++; locus.tabs = 0; locus.characters = 0; break;
			case '\t': locus.tabs++; break;
			default: locus.characters++; break;
			};
			next++;
		}
		void advanceToPtr(const char* newNext)
		{
			while(next != newNext)
			{
				advance();
			};
		}

		// Tries to skip to the next instance of a character, excluding instances between nested parentheses.
		// If successful, returns true. Will stop skipping and return false if it hits the end of the string or
		// a closing parenthesis that doesn't match a skipped opening parenthesis.
		bool advancePastNext(char c)
		{
			uint32 parenthesesDepth = 0;
			while(true)
			{
				auto nextChar = peek();
				if(nextChar == 0)
				{
					return false;
				}
				else if(nextChar == c && parenthesesDepth == 0)
				{
					advance();
					return true;
				}
				else if(nextChar == '(')
				{
					++parenthesesDepth;
				}
				else if(nextChar == ')')
				{
					if(parenthesesDepth == 0)
					{
						return false;
					}
					--parenthesesDepth;
				}
				advance();
			};
		}
	};

	bool isWhitespace(char c)
	{
		return c == ' ' || c == '\t' || c == '\r' || c == '\n';
	}

	bool isDigit(char c)
	{
		return c >= '0' && c <= '9';
	}

	bool isSymbolCharacter(char c)
	{
		return !isWhitespace(c) && c != '(' && c != ')' && c != ';' && c != '\"' && c != '=';
	}

	bool parseHexit(char c,char& outValue)
	{
		if(c >= '0' && c <= '9')
		{
			outValue = c - '0';
			return true;
		}
		else if(c >= 'a' && c <= 'f')
		{
			outValue = c - 'a' + 10;
			return true;
		}
		else if(c >= 'A' && c <= 'F')
		{
			outValue = c - 'A' + 10;
			return true;
		}
		else
		{
			return false;
		}
	}

	uint64 shlSaturate(uint64 left,uint64 right)
	{
		return right >= 64 ? 0 : left << right;
	}
	uint64 shrSaturate(uint64 left,uint64 right)
	{
		return right >= 64 ? 0 : left >> right;
	}

	struct ParseContext
	{
		StreamState& state;
		Memory::Arena& arena;
		const SymbolIndexMap& symbolIndexMap;

		ParseContext(StreamState& inState,Memory::Arena& inArena,const SymbolIndexMap& inSymbolIndexMap)
		: state(inState), arena(inArena), symbolIndexMap(inSymbolIndexMap) {}
		
		bool parseCharEscapeCode(char& outCharacter)
		{
			switch(state.peek())
			{
			case 'n':	outCharacter = '\n'; break;
			case 't': outCharacter = '\t'; break;
			case '\\': outCharacter = '\\'; break;
			case '\'': outCharacter = '\''; break;
			case '\"': outCharacter = '\"'; break;
			default:
			{
				char firstValue;
				if(!parseHexit(state.peek(),firstValue))
				{
					return false;
				}
				state.advance();
				char secondValue;
				if(!parseHexit(state.peek(),secondValue))
				{
					return false;
				}
				outCharacter = firstValue * 16 + secondValue;
				break;
			}
			}
			return true;
		}

		bool parseChar(char c)
		{
			if(state.peek() != c) { return false; }
			else
			{
				state.consume();
				return true;
			}
		}

		Node* createError(const Core::TextFileLocus& locus,const char* message)
		{
			auto result = new(arena) Node(locus,NodeType::Error);
			result->error = message;
			return result;
		}

		Node* parseQuotedString()
		{
			state.advance();

			Memory::ArenaString string;
			while(true)
			{
				const char nextChar = state.peek();
				if(nextChar == '\n' || nextChar == 0)
				{
					string.reset(arena);
					auto locus = state.getLocus();
					state.advancePastNext('\"');
					return createError(locus,"unexpected newline or end of file in quoted string");
				}
				else if(nextChar == '\\')
				{
					state.advance();
					char escapedChar;
					if(!parseCharEscapeCode(escapedChar))
					{
						string.reset(arena);
						auto locus = state.getLocus();
						state.advancePastNext('\"');
						return createError(locus,"invalid escape code in quoted string");
					}
					string.append(arena,escapedChar);
					state.advance();
				}
				else
				{
					state.advance();
					if(nextChar == '\"') { break; }
					else { string.append(arena,nextChar); }
				}
			}

			string.shrink(arena);
			auto node = new(arena)Node(state.getLocus(),NodeType::String);
			node->string = string.c_str();
			node->stringLength = string.length();
			node->endLocus = state.getLocus();
			return node;
		}

		bool parseKeyword(const char* keyword)
		{
			StreamState savedState = state;
			for(uintptr charIndex = 0;keyword[charIndex];++charIndex)
			{
				if(state.consume() != keyword[charIndex])
				{
					state = savedState;
					return false;
				}
			}
			return true;
		}

		uint64 parseHexInteger(uint64& outValue)
		{
			StreamState savedState = state;
			outValue = 0;
			uintptr numMatchedCharacters = 0;
			while(true)
			{
				char hexit = 0;
				if(!parseHexit(state.peek(),hexit))
				{
					return numMatchedCharacters;
				}
				state.consume();
				++numMatchedCharacters;

				if(outValue > std::numeric_limits<uint64>::max() / 16)
				{
					state = savedState;
					return 0;
				}
				outValue *= 16;
				outValue += hexit;
			}
		}

		bool parseDecimalInteger(uint64& outValue)
		{
			errno = 0;
			const char* i64End = state.next;
			outValue = std::strtoull(state.next,const_cast<char**>(&i64End),10);
			if(i64End == state.next) { return false; }
			if(errno == ERANGE) { return false; }
			state.advanceToPtr(i64End);
			return true;
		}
	
		Node* parseNaN(const Core::TextFileLocus& startLocus,bool isNegative)
		{
			Floats::F64Components f64Components;
			f64Components.bits.sign = isNegative;
			f64Components.bits.exponent = 0x7ff;
			f64Components.bits.significand = 1ull << 51;

			Floats::F32Components f32Components;
			f32Components.bits.sign = isNegative;
			f32Components.bits.exponent = 0xff;
			f32Components.bits.significand = 1ull << 22;

			if(state.peek() == '(')
			{
				state.consume();

				uint64 significandBits = 0;
				if(!parseKeyword("0x"))
				{
					state.advancePastNext(')');
					return createError(state.getLocus(),"expected hexadecimal NaN significand");
				}
				if(!parseHexInteger(significandBits))
				{
					return createError(state.getLocus(),"expected hexadecimal NaN significand");
				}
				if(significandBits == 0)
				{
					return createError(state.getLocus(),"NaN significand must be non-zero");
				}
				if(state.consume() != ')')
				{
					return createError(state.getLocus(),"expected ')'");
				}

				f64Components.bits.significand = significandBits;
				f32Components.bits.significand = significandBits;
			}

			auto node = new(arena) Node(startLocus,NodeType::Float);
			node->endLocus = state.getLocus();
			node->f64 = f64Components.value;
			node->f32 = f32Components.value;
			return node;
		}

		Node* parseInfinity(const Core::TextFileLocus& startLocus,bool isNegative)
		{
			// Floating point infinite is represented by max exponent with a zero significand.
			Floats::F64Components f64Components;
			f64Components.bits.sign = isNegative ? 1 : 0;
			f64Components.bits.exponent = 0x7ff;
			f64Components.bits.significand = 0;
			Floats::F32Components f32Components;
			f32Components.bits.sign = isNegative ? 1 : 0;
			f32Components.bits.exponent = 0xff;
			f32Components.bits.significand = 0;

			auto node = new(arena) Node(startLocus,NodeType::Float);
			node->endLocus = state.getLocus();
			node->f64 = f64Components.value;
			node->f32 = f32Components.value;
			return node;
		}

		Node* parseHexNumber(const Core::TextFileLocus& startLocus,bool isNegative)
		{
			uint64 integerPart;
			if(!parseHexInteger(integerPart)) { return createError(state.getLocus(),"expected hex digits"); }

			bool hasDecimalPoint = false;
			uint64 fractionalPart = 0;
			if(parseKeyword("."))
			{
				// Shift the fractional part so the MSB is in the MSB of the float64 significand.
				auto numFractionalHexits = parseHexInteger(fractionalPart);
				fractionalPart <<= 52 - numFractionalHexits * 4;
				hasDecimalPoint = true;
			}
			
			int64 exponent = 0;
			bool hasExponent = false;
			if(parseKeyword("p") || parseKeyword("P"))
			{
				hasExponent = true;

				// Parse an optional exponent sign.
				bool isExponentNegative = false;
				if(state.peek() == '-' || state.peek() == '+') { isExponentNegative = state.consume() == '-'; }

				// Parse a decimal exponent.
				uint64 unsignedExponent = 0;
				if(!parseDecimalInteger(unsignedExponent))
				{
					return createError(state.getLocus(),"expected exponent decimal");
				}
				exponent = isExponentNegative ? -(int64)unsignedExponent : unsignedExponent;
				
				if(exponent < -1022 || exponent > 1023)
				{
					return createError(state.getLocus(),"exponent must be between -1022 and +1023");
				}
			}
		
			// If there wasn't a fractional part, or exponent, or negative zero, then just create an integer node.
			if(!fractionalPart && !exponent && !(isNegative && !integerPart && (hasDecimalPoint || hasExponent)))
			{
				auto node = new(arena) Node(state.getLocus(),isNegative ? NodeType::SignedInt : NodeType::UnsignedInt);
				node->endLocus = state.getLocus();
				node->i64 = isNegative ? -(int64)integerPart : integerPart;
				return node;
			}
			else
			{
				if(!integerPart)
				{
					if(!fractionalPart)
					{
						// If both the integer and fractional part are zero, just return zero.
						auto node = new(arena) Node(state.getLocus(),NodeType::Float);
						node->endLocus = state.getLocus();
						node->f64 = isNegative ? -0.0 : 0.0;
						node->f32 = isNegative ? -0.0f : 0.0f;
						return node;
					}
					else if(exponent != -1022)
					{
						return createError(startLocus,"exponent on subnormal hexadecimal float must be -1022");
					}
					else
					{
						// For subnormals (integerPart=0 fractionalPart!=0 exponent=-1022), change the encoded exponent to -1023.
						exponent = -1023;
					}
				}
				else if(integerPart != 1)
				{
					return createError(startLocus,"hexadecimal float must start with 0x1. or 0x0.");
				}

				// Encode the float and create a node for it.
				Floats::F64Components f64Components;
				f64Components.bits.sign = isNegative ? 1 : 0;
				f64Components.bits.exponent = exponent + 1023;
				f64Components.bits.significand = fractionalPart;
				auto node = new(arena) Node(state.getLocus(),NodeType::Float);
				node->endLocus = state.getLocus();
				node->f64 = f64Components.value;
				node->f32 = (float32)f64Components.value;
				return node;
			}
		}

		Node* parseNumber()
		{
			/*
				Syntax from the WebAssembly/spec interpreter number lexer
				let sign = ('+' | '-')?
				let num = sign digit+
				let hexnum = sign "0x" hexdigit+
				let int = num | hexnum
				let float = (num '.' digit+)
				  | num ('.' digit+)? ('e' | 'E') num
				  | sign "0x" hexdigit+ '.'? hexdigit* 'p' sign digit+
				  | sign "infinity"
				  | sign "nan"
				  | sign "nan(0x" hexdigit+ ")"
			*/
			auto startLocus = state.getLocus();

			// Parse the sign.
			bool isNegative = false;
			if(state.peek() == '-' || state.peek() == '+')
			{
				isNegative = state.consume() == '-';
			}

			// Handle nan, infinity, and hexadecimal numbers.
			if(parseKeyword("nan")) { return parseNaN(startLocus,isNegative); }
			else if(parseKeyword("infinity")) { return parseInfinity(startLocus,isNegative); }
			else if(parseKeyword("0x") || parseKeyword("0X")) { return parseHexNumber(startLocus,isNegative); }
			else
			{
				// For decimals, just use the std float parsing code.
				const char* f64End = state.next;
				float64 f64 = std::strtod(state.next,const_cast<char**>(&f64End));
				const char* i64End = state.next;
				uint64 i64 = std::strtoull(state.next,const_cast<char**>(&i64End),0);

				// Between the float and the integer parser, use whichever consumed more input, favoring integers if they're equal.
				if(f64End > i64End)
				{
					state.advanceToPtr(f64End);
					auto node = new(arena) Node(state.getLocus(),NodeType::Float);
					node->endLocus = state.getLocus();
					node->f64 = isNegative ? -f64 : f64;
					node->f32 = (float32)node->f64;
					return node;
				}
				else
				{
					state.advanceToPtr(i64End);
					auto node = new(arena) Node(state.getLocus(),isNegative ? NodeType::SignedInt : NodeType::UnsignedInt);
					node->endLocus = state.getLocus();
					node->i64 = isNegative ? -(int64)i64 : i64;
					return node;
				}
			}
		}
	
		Node* parseAttribute(Node* symbolNode)
		{
			state.consume();

			auto value = parseNode();

			auto attributeNode = new(arena) Node(symbolNode->startLocus,NodeType::Attribute);
			attributeNode->children = symbolNode;
			attributeNode->children->nextSibling = value;
			attributeNode->endLocus = state.getLocus();
			return attributeNode;
		}

		Node* parseSymbol()
		{
			auto startLocus = state.getLocus();

			Memory::ArenaString string;
			do
			{
				string.append(arena,state.peek());
				state.advance();
			}
			while(isSymbolCharacter(state.peek()));

			// If the symbol is nan or infinity, parse it as a number.
			if(!strcmp(string.c_str(),"nan")) { return parseNaN(startLocus,false); }
			else if(!strcmp(string.c_str(),"infinity")) { return parseInfinity(startLocus,false); }

			// Look up the symbol string in the index map.
			auto symbolIndexIt = symbolIndexMap.find(string.c_str());
			Node* symbolNode;
			if(symbolIndexIt == symbolIndexMap.end())
			{
				string.shrink(arena);
				symbolNode = new(arena)Node(startLocus,NodeType::UnindexedSymbol);
				symbolNode->endLocus = state.getLocus();
				symbolNode->string = string.c_str();
				symbolNode->stringLength = string.length();
			}
			else
			{
				// If the symbol was in the index map, discard the memory for the string and just store it as an index.
				string.reset(arena);
				symbolNode = new(arena)Node(startLocus,NodeType::Symbol);
				symbolNode->endLocus = state.getLocus();
				symbolNode->symbol = symbolIndexIt->second;
			}

			// If the symbol is followed by an equals sign, parse an attribute.
			if(state.peek() == '=') { return parseAttribute(symbolNode); }
			else { return symbolNode; }
		}
	
		Node* parseNode()
		{
			while(true)
			{
				const char nextChar = state.peek();
				if(nextChar == 0)
				{
					return nullptr;
				}
				else if(isWhitespace(nextChar))
				{
					// Skip whitespace.
					state.advance();
				}
				else if(nextChar == ';')
				{
					// Parse a line comment.
					state.advance();
					if(state.peek() != ';')
					{
						throw new FatalParseException(state.getLocus(),std::string("expected ';' following ';' but found '") + nextChar + "'");
					}

					while(state.peek() != '\n' && state.peek() != 0) state.advance();
				}
				else if(nextChar == '(')
				{
					state.advance();

					if(state.peek() == ';')
					{
						// Parse a block comment.
						do
						{
							if(!state.advancePastNext(';'))
							{
								throw new FatalParseException(state.getLocus(),"reached end of file while parsing block comment");
							}
						}
						while(state.peek() != ')');
						state.advance();
					}
					else
					{
						// Recursively parse child nodes.
						auto newNode = new(arena)Node(state.getLocus());
						parseNodeSequence(&newNode->children);
						if(state.peek() != ')')
						{
							throw new FatalParseException(state.getLocus(),std::string("expected ')' following S-expression child nodes but found '") + nextChar + "'");
						}
						state.advance();
						newNode->endLocus = state.getLocus();
						return newNode;
					}
				}
				else if(nextChar == '\"')
				{
					// Parse a quoted symbol.
					return parseQuotedString();
				}
				else if(isDigit(nextChar) || nextChar == '+' || nextChar == '-' || nextChar == '.')
				{
					// Parse a number.
					return parseNumber();
				}
				else if(isSymbolCharacter(nextChar))
				{
					// Parse a symbol.
					return parseSymbol();
				}
				else
				{
					return nullptr;
				}
			}
		}

		void parseNodeSequence(Node** nextNodePtr)
		{
			while(true)
			{
				auto node = parseNode();
				if(!node) { break; }
				*nextNodePtr = node;
				nextNodePtr = &(*nextNodePtr)->nextSibling;
			}
		}
	};

	Node* parse(const char* string,Memory::Arena& arena,const SymbolIndexMap& symbolIndexMap)
	{
		StreamState state(string);
		ParseContext parseContext(state,arena,symbolIndexMap);
		try
		{
			Node* firstRootNode = nullptr;
			parseContext.parseNodeSequence(&firstRootNode);
			return firstRootNode;
		}
		catch(FatalParseException* exception)
		{
			auto result = parseContext.createError(exception->locus,arena.copyToArena(exception->message.c_str(),exception->message.length() + 1));
			delete exception;
			return result;
		}
	}

	char nibbleToHexChar(uint8 value) { return value < 10 ? ('0' + value) : 'a' + value - 10; }

	std::string escapeString(const char* string,size_t numChars)
	{
		std::string result;
		for(uintptr charIndex = 0;charIndex < numChars;++charIndex)
		{
			auto c = string[charIndex];
			if(c == '\\') { result += "\\\\"; }
			else if(c == '\"') { result += "\\\""; }
			else if(c == '\n') { result += "\\n"; }
			else if(c < 0x20 || c > 0x7e)
			{
				result += '\\';
				result += nibbleToHexChar((c & 0xf0) >> 4);
				result += nibbleToHexChar((c & 0x0f) >> 0);
			}
			else { result += c; }
		}
		return result;
	}

	bool printRecursive(SExp::Node* initialNode,const char* symbolStrings[],std::string& outString,const std::string& newline);

	std::string printNode(SExp::Node* node,const char* symbolStrings[],const std::string& newline,bool& outHasMultiLineSubtree)
	{
		switch(node->type)
		{
		case NodeType::Tree:
		{
			std::string subtreeString;
			subtreeString += "(";
			bool isSubtreeMultiLine = printRecursive(node->children,symbolStrings,subtreeString,newline + '\t');
			if(isSubtreeMultiLine) { subtreeString += newline; outHasMultiLineSubtree = true; }
			subtreeString += ")";
			return subtreeString;
		}
		case NodeType::Attribute:
		{
			std::string attributeString = printNode(node->children,symbolStrings,newline,outHasMultiLineSubtree);
			attributeString += "=";
			assert(node->children->nextSibling);
			attributeString += printNode(node->children->nextSibling,symbolStrings,newline,outHasMultiLineSubtree);
			return attributeString;
		}
		case NodeType::Symbol: return symbolStrings[node->symbol];
		case NodeType::UnindexedSymbol: return node->string;
		case NodeType::String: return std::string() + '\"' + escapeString(node->string,node->stringLength) + '\"';
		case NodeType::Error: return node->error;
		case NodeType::SignedInt: return std::to_string(node->i64);
		case NodeType::UnsignedInt: return std::to_string(node->u64);
		case NodeType::Float: return Floats::asString(node->f64);
		default: throw;
		};
	}

	bool printRecursive(SExp::Node* initialNode,const char* symbolStrings[],std::string& outString,const std::string& newline)
	{
		SExp::Node* node = initialNode;
		std::vector<std::string> childStrings;
		bool hasMultiLineSubtree = false;
		do
		{
			childStrings.emplace_back(std::move(printNode(node,symbolStrings,newline,hasMultiLineSubtree)));
			node = node->nextSibling;
		}
		while(node);
		
		size_t totalChildLength = 0;
		for(auto string : childStrings) { totalChildLength += string.length(); }
		auto isMultiLine = hasMultiLineSubtree || totalChildLength > 120;

		for(uintptr childIndex = 0;childIndex < childStrings.size();++childIndex)
		{
			outString += childStrings[childIndex];
			if(childIndex + 1 < childStrings.size()) { outString += isMultiLine ? newline : " "; }
		}

		return isMultiLine;
	}

	std::string print(SExp::Node* rootNode,const char* symbolStrings[])
	{
		std::string result;
		printRecursive(rootNode,symbolStrings,result,"\n");
		return result;
	}
}
