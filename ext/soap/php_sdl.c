/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2004 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.0 of the PHP license,       |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_0.txt.                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Brad Lafountain <rodif_bl@yahoo.com>                        |
  |          Shane Caraveo <shane@caraveo.com>                           |
  |          Dmitry Stogov <dmitry@zend.com>                             |
  +----------------------------------------------------------------------+
*/
/* $Id$ */

#include "php_soap.h"
#include "libxml/uri.h"

typedef struct sdlCtx {
	sdlPtr root;
	HashTable messages;
	HashTable bindings;
	HashTable portTypes;
	HashTable services;
} sdlCtx;

static void delete_binding(void *binding);
static void delete_function(void *function);
static void delete_paramater(void *paramater);
static void delete_header(void *header);
static void delete_document(void *doc_ptr);

encodePtr get_encoder_from_prefix(sdlPtr sdl, xmlNodePtr data, const char *type)
{
	encodePtr enc = NULL;
	TSRMLS_FETCH();

	enc = get_conversion_from_type(data, type);
	if (enc == NULL && sdl) {
		enc = get_conversion_from_type_ex(sdl->encoders, data, type);
	}
	return enc;
}

static sdlTypePtr get_element(sdlPtr sdl, xmlNodePtr node, const char *type)
{
	sdlTypePtr ret = NULL;
	TSRMLS_FETCH();

	if (sdl && sdl->elements) {
		xmlNsPtr nsptr;
		char *ns, *cptype;
		sdlTypePtr *sdl_type;

		parse_namespace(type, &cptype, &ns);
		nsptr = xmlSearchNs(node->doc, node, ns);
		if (nsptr != NULL) {
			smart_str nscat = {0};

			smart_str_appends(&nscat, nsptr->href);
			smart_str_appendc(&nscat, ':');
			smart_str_appends(&nscat, cptype);
			smart_str_0(&nscat);

			if (zend_hash_find(sdl->elements, nscat.c, nscat.len + 1, (void **)&sdl_type) == SUCCESS) {
				ret = *sdl_type;
			} else if (zend_hash_find(sdl->elements, (char*)type, strlen(type) + 1, (void **)&sdl_type) == SUCCESS) {
				ret = *sdl_type;
			}
			smart_str_free(&nscat);
		} else {
			if (zend_hash_find(sdl->elements, (char*)type, strlen(type) + 1, (void **)&sdl_type) == SUCCESS) {
				ret = *sdl_type;
			}
		}

		if (cptype) {efree(cptype);}
		if (ns) {efree(ns);}
	}
	return ret;
}

encodePtr get_encoder(sdlPtr sdl, const char *ns, const char *type)
{
	encodePtr enc = NULL;
	char *nscat;

	nscat = emalloc(strlen(ns) + strlen(type) + 2);
	sprintf(nscat, "%s:%s", ns, type);

	enc = get_encoder_ex(sdl, nscat);

	efree(nscat);
	return enc;
}

encodePtr get_encoder_ex(sdlPtr sdl, const char *nscat)
{
	encodePtr enc = NULL;
	TSRMLS_FETCH();

	enc = get_conversion_from_href_type(nscat);
	if (enc == NULL && sdl) {
		enc = get_conversion_from_href_type_ex(sdl->encoders, nscat, strlen(nscat));
	}
	return enc;
}

sdlBindingPtr get_binding_from_type(sdlPtr sdl, int type)
{
	sdlBindingPtr *binding;

	if (sdl == NULL) {
		return NULL;
	}

	for (zend_hash_internal_pointer_reset(sdl->bindings);
		zend_hash_get_current_data(sdl->bindings, (void **) &binding) == SUCCESS;
		zend_hash_move_forward(sdl->bindings)) {
		if ((*binding)->bindingType == type) {
			return *binding;
		}
	}
	return NULL;
}

sdlBindingPtr get_binding_from_name(sdlPtr sdl, char *name, char *ns)
{
	sdlBindingPtr binding = NULL;
	smart_str key = {0};

	smart_str_appends(&key, ns);
	smart_str_appendc(&key, ':');
	smart_str_appends(&key, name);
	smart_str_0(&key);

	zend_hash_find(sdl->bindings, key.c, key.len, (void **)&binding);

	smart_str_free(&key);
	return binding;
}

static void load_wsdl_ex(char *struri, sdlCtx *ctx, int include)
{
	sdlPtr tmpsdl = ctx->root;
	xmlDocPtr wsdl;
	xmlNodePtr root, definitions, trav;
	xmlAttrPtr targetNamespace;

	if (zend_hash_exists(&tmpsdl->docs, struri, strlen(struri)+1)) {
	  return;
	}

	/* TODO: WSDL Caching */

	wsdl = soap_xmlParseFile(struri);

	if (!wsdl) {
		php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Couldn't load from '%s'", struri);
	}

	zend_hash_add(&tmpsdl->docs, struri, strlen(struri)+1, (void**)&wsdl, sizeof(xmlDocPtr), NULL);

	root = wsdl->children;
	definitions = get_node(root, "definitions");
	if (!definitions) {
		if (include) {
			xmlNodePtr schema = get_node(root, "schema");
			if (schema) {
				load_schema(tmpsdl, schema);
				return;
			}
		}
		php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Couldn't find <definitions> in '%s'", struri);
	}

	if (!include) {
		targetNamespace = get_attribute(definitions->properties, "targetNamespace");
		if (targetNamespace) {
			tmpsdl->target_ns = strdup(targetNamespace->children->content);
		}
	}

	trav = definitions->children;
	while (trav != NULL) {
		if (node_is_equal(trav,"types")) {
			/* TODO: Only one "types" is allowed */
			xmlNodePtr trav2 = trav->children;
			xmlNodePtr schema;

			FOREACHNODE(trav2, "schema", schema) {
				load_schema(tmpsdl, schema);
			}
			ENDFOREACH(trav2);

		} else if (node_is_equal(trav,"import")) {
			/* TODO: namespace ??? */
			xmlAttrPtr tmp = get_attribute(trav->properties, "location");
			if (tmp) {
			  xmlChar *uri;
				xmlChar *base = xmlNodeGetBase(trav->doc, trav);

				if (base == NULL) {
			    uri = xmlBuildURI(tmp->children->content, trav->doc->URL);
				} else {
    			uri = xmlBuildURI(tmp->children->content, base);
			    xmlFree(base);
				}
				load_wsdl_ex(uri, ctx, 1);
		    xmlFree(uri);
			}

		} else if (node_is_equal(trav,"message")) {
			xmlAttrPtr name = get_attribute(trav->properties, "name");
			if (name && name->children && name->children->content) {
				if (zend_hash_add(&ctx->messages, name->children->content, strlen(name->children->content)+1,&trav, sizeof(xmlNodePtr), NULL) != SUCCESS) {
					php_error(E_ERROR,"SOAP-ERROR: Parsing WSDL: <message> '%s' already defined",name->children->content);
				}
			} else {
				php_error(E_ERROR,"SOAP-ERROR: Parsing WSDL: <message> hasn't name attribute");
			}

		} else if (node_is_equal(trav,"portType")) {
			xmlAttrPtr name = get_attribute(trav->properties, "name");
			if (name && name->children && name->children->content) {
				if (zend_hash_add(&ctx->portTypes, name->children->content, strlen(name->children->content)+1,&trav, sizeof(xmlNodePtr), NULL) != SUCCESS) {
					php_error(E_ERROR,"SOAP-ERROR: Parsing WSDL: <portType> '%s' already defined",name->children->content);
				}
			} else {
				php_error(E_ERROR,"SOAP-ERROR: Parsing WSDL: <portType> hasn't name attribute");
			}

		} else if (node_is_equal(trav,"binding")) {
			xmlAttrPtr name = get_attribute(trav->properties, "name");
			if (name && name->children && name->children->content) {
				if (zend_hash_add(&ctx->bindings, name->children->content, strlen(name->children->content)+1,&trav, sizeof(xmlNodePtr), NULL) != SUCCESS) {
					php_error(E_ERROR,"SOAP-ERROR: Parsing WSDL: <binding> '%s' already defined",name->children->content);
				}
			} else {
				php_error(E_ERROR,"SOAP-ERROR: Parsing WSDL: <binding> hasn't name attribute");
			}

		} else if (node_is_equal(trav,"service")) {
			xmlAttrPtr name = get_attribute(trav->properties, "name");
			if (name && name->children && name->children->content) {
				if (zend_hash_add(&ctx->services, name->children->content, strlen(name->children->content)+1,&trav, sizeof(xmlNodePtr), NULL) != SUCCESS) {
					php_error(E_ERROR,"SOAP-ERROR: Parsing WSDL: <service> '%s' already defined",name->children->content);
				}
			} else {
				php_error(E_ERROR,"SOAP-ERROR: Parsing WSDL: <service> hasn't name attribute");
			}
		}
		trav = trav->next;
	}
}

static void wsdl_soap_binding_body(sdlCtx* ctx, xmlNodePtr node, char* wsdl_soap_namespace, sdlSoapBindingFunctionBody *binding)
{
	xmlNodePtr body, trav, header;
	xmlAttrPtr tmp;

	body = get_node_ex(node->children, "body", wsdl_soap_namespace);
	if (body) {
		tmp = get_attribute(body->properties, "use");
		if (tmp && !strncmp(tmp->children->content, "literal", sizeof("literal"))) {
			binding->use = SOAP_LITERAL;
		} else {
			binding->use = SOAP_ENCODED;
		}

		tmp = get_attribute(body->properties, "namespace");
		if (tmp) {
			binding->ns = strdup(tmp->children->content);
		}

		tmp = get_attribute(body->properties, "parts");
		if (tmp) {
			binding->parts = strdup(tmp->children->content);
		}

		if (binding->use == SOAP_ENCODED) {
			tmp = get_attribute(body->properties, "encodingStyle");
			if (tmp &&
			    strncmp(tmp->children->content,SOAP_1_1_ENC_NAMESPACE,sizeof(SOAP_1_1_ENC_NAMESPACE)) != 0 &&
		  	  strncmp(tmp->children->content,SOAP_1_2_ENC_NAMESPACE,sizeof(SOAP_1_2_ENC_NAMESPACE)) != 0) {
				php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Unknown encodingStyle '%s'",tmp->children->content);
			} else if (tmp == NULL) {
				php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Unspecified encodingStyle");
			} else {
				binding->encodingStyle = strdup(tmp->children->content);
			}
		}
	}

	/* Process <soap:header> elements */
	trav = node->children;
	FOREACHNODEEX(trav, "header", wsdl_soap_namespace, header) {
		xmlAttrPtr tmp;
		xmlNodePtr *message, part;
		char *ctype, *ns;
		sdlSoapBindingFunctionHeaderPtr h;
		smart_str key = {0};

		tmp = get_attribute(header->properties, "message");
		if (!tmp) {
			php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Missing message attribute for <header>");
		}
		parse_namespace(tmp->children->content, &ctype, &ns);
		if (zend_hash_find(&ctx->messages, ctype, strlen(ctype)+1, (void**)&message) != SUCCESS) {
			php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Missing <message> with name '%s'", tmp->children->content);
		}
		if (ctype) {efree(ctype);}
		if (ns) {efree(ns);}

		tmp = get_attribute(header->properties, "part");
		if (!tmp) {
			php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Missing part attribute for <header>");
		}
		part = get_node_with_attribute((*message)->children, "part", "name", tmp->children->content);
		if (!part) {
			php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Missing part '%s' in <message>",tmp->children->content);
		}

		h = malloc(sizeof(sdlSoapBindingFunctionHeader));
		memset(h, 0, sizeof(sdlSoapBindingFunctionHeader));
		h->name = strdup(tmp->children->content);

		tmp = get_attribute(part->properties, "type");
		if (tmp != NULL) {
			h->encode = get_encoder_from_prefix(ctx->root, part, tmp->children->content);
		} else {
			tmp = get_attribute(part->properties, "element");
			if (tmp != NULL) {
				h->element = get_element(ctx->root, part, tmp->children->content);
				if (h->element) {
					h->encode = h->element->encode;
				}
			}
		}

		tmp = get_attribute(header->properties, "use");
		if (tmp && !strncmp(tmp->children->content, "encoded", sizeof("encoded"))) {
			h->use = SOAP_ENCODED;
		} else {
			h->use = SOAP_LITERAL;
		}

		tmp = get_attribute(header->properties, "namespace");
		if (tmp) {
			h->ns = strdup(tmp->children->content);
		}

		if (h->use == SOAP_ENCODED) {
			tmp = get_attribute(header->properties, "encodingStyle");
			if (tmp &&
			    strncmp(tmp->children->content,SOAP_1_1_ENC_NAMESPACE,sizeof(SOAP_1_1_ENC_NAMESPACE)) != 0 &&
			    strncmp(tmp->children->content,SOAP_1_2_ENC_NAMESPACE,sizeof(SOAP_1_2_ENC_NAMESPACE)) != 0) {
				php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Unknown encodingStyle '%s'",tmp->children->content);
			} else if (tmp == NULL) {
				php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Unspecified encodingStyle");
			} else {
				h->encodingStyle = strdup(tmp->children->content);
			}
		}

		if (binding->headers == NULL) {
			binding->headers = malloc(sizeof(HashTable));
			zend_hash_init(binding->headers, 0, NULL, delete_header, 1);
		}

		if (h->ns) {
			smart_str_appends(&key,h->ns);
			smart_str_appendc(&key,':');
		}
		smart_str_appends(&key,h->name);
		smart_str_0(&key);
		if (zend_hash_add(binding->headers, key.c, key.len+1, (void**)&h, sizeof(sdlSoapBindingFunctionHeaderPtr), NULL) != SUCCESS) {
			delete_header((void**)&h);
		}
		smart_str_free(&key);

	}
	ENDFOREACH(trav);
}

static HashTable* wsdl_message(sdlCtx *ctx, char* message_name)
{
	xmlNodePtr trav, part, message = NULL, *tmp;
	HashTable* parameters = NULL;
	char *ns, *ctype;

	parse_namespace(message_name, &ctype, &ns);
	if (zend_hash_find(&ctx->messages, ctype, strlen(ctype)+1, (void**)&tmp) != SUCCESS) {
		php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Missing <message> with name '%s'", message->children->content);
	}
	message = *tmp;
	if (ctype) {efree(ctype);}
	if (ns) {efree(ns);}

	parameters = malloc(sizeof(HashTable));
	zend_hash_init(parameters, 0, NULL, delete_paramater, 1);

	trav = message->children;
	FOREACHNODE(trav, "part", part) {
		xmlAttrPtr element, type, name;
		sdlParamPtr param;

		param = malloc(sizeof(sdlParam));
		memset(param,0,sizeof(sdlParam));
		param->order = 0;

		name = get_attribute(part->properties, "name");
		if (name == NULL) {
			php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: No name associated with <part> '%s'", message->name);
		}

		param->paramName = strdup(name->children->content);

		type = get_attribute(part->properties, "type");
		if (type != NULL) {
			param->encode = get_encoder_from_prefix(ctx->root, part, type->children->content);
		} else {
			element = get_attribute(part->properties, "element");
			if (element != NULL) {
				param->element = get_element(ctx->root, part, element->children->content);
				if (param->element) {
					param->encode = param->element->encode;
				}
			}
		}

		zend_hash_next_index_insert(parameters, &param, sizeof(sdlParamPtr), NULL);
	}
	ENDFOREACH(trav);
	return parameters;
}

static sdlPtr load_wsdl(char *struri)
{
	sdlCtx ctx;
	int i,n;

	ctx.root = malloc(sizeof(sdl));
	memset(ctx.root, 0, sizeof(sdl));
	ctx.root->source = strdup(struri);
	zend_hash_init(&ctx.root->docs, 0, NULL, delete_document, 1);
	zend_hash_init(&ctx.root->functions, 0, NULL, delete_function, 1);

	zend_hash_init(&ctx.messages, 0, NULL, NULL, 0);
	zend_hash_init(&ctx.bindings, 0, NULL, NULL, 0);
	zend_hash_init(&ctx.portTypes, 0, NULL, NULL, 0);
	zend_hash_init(&ctx.services,  0, NULL, NULL, 0);

	load_wsdl_ex(struri,&ctx, 0);
	schema_pass2(ctx.root);

	n = zend_hash_num_elements(&ctx.services);
	if (n > 0) {
		zend_hash_internal_pointer_reset(&ctx.services);
		for (i = 0; i < n; i++) {
			xmlNodePtr *tmp, service;
			xmlNodePtr trav, port;

			zend_hash_get_current_data(&ctx.services, (void **)&tmp);
			service = *tmp;

			trav = service->children;
			FOREACHNODE(trav, "port", port) {
				xmlAttrPtr type, name, bindingAttr, location;
				xmlNodePtr portType, operation;
				xmlNodePtr address, binding, trav2;
				char *ns, *ctype;
				sdlBindingPtr tmpbinding;
				char *wsdl_soap_namespace = NULL;

				tmpbinding = malloc(sizeof(sdlBinding));
				memset(tmpbinding, 0, sizeof(sdlBinding));

				bindingAttr = get_attribute(port->properties, "binding");
				if (bindingAttr == NULL) {
					php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: No binding associated with <port>");
				}

				/* find address and figure out binding type */
				address = get_node(port->children, "address");
				if (!address) {
					php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: No address associated with <port>");
				}

				location = get_attribute(address->properties, "location");
				if (!location) {
					php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: No location associated with <port>");
				}

				tmpbinding->location = strdup(location->children->content);

				if (address->ns) {
					if (!strncmp(address->ns->href, WSDL_SOAP11_NAMESPACE, sizeof(WSDL_SOAP11_NAMESPACE))) {
						wsdl_soap_namespace = WSDL_SOAP11_NAMESPACE;
						tmpbinding->bindingType = BINDING_SOAP;
					} else if (!strncmp(address->ns->href, WSDL_SOAP12_NAMESPACE, sizeof(WSDL_SOAP12_NAMESPACE))) {
						wsdl_soap_namespace = WSDL_SOAP12_NAMESPACE;
						tmpbinding->bindingType = BINDING_SOAP;
					} else if (!strncmp(address->ns->href, RPC_SOAP12_NAMESPACE, sizeof(RPC_SOAP12_NAMESPACE))) {
						wsdl_soap_namespace = RPC_SOAP12_NAMESPACE;
						tmpbinding->bindingType = BINDING_SOAP;
					} else if (!strncmp(address->ns->href, WSDL_HTTP11_NAMESPACE, sizeof(WSDL_HTTP11_NAMESPACE))) {
						tmpbinding->bindingType = BINDING_HTTP;
					} else if (!strncmp(address->ns->href, WSDL_HTTP12_NAMESPACE, sizeof(WSDL_HTTP12_NAMESPACE))) {
						tmpbinding->bindingType = BINDING_HTTP;
					} else {
						php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: PHP-SOAP doesn't support binding '%s'",address->ns->href);
					}
				} else {
					php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Unknown binding type");
				}

				parse_namespace(bindingAttr->children->content, &ctype, &ns);
				if (zend_hash_find(&ctx.bindings, ctype, strlen(ctype)+1, (void*)&tmp) != SUCCESS) {
					php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: No <binding> element with name '%s'", ctype);
				}
				binding = *tmp;

				if (ns) {efree(ns);}
				if (ctype) {efree(ctype);}

				if (tmpbinding->bindingType == BINDING_SOAP) {
					sdlSoapBindingPtr soapBinding;
					xmlNodePtr soapBindingNode;
					xmlAttrPtr tmp;

					soapBinding = malloc(sizeof(sdlSoapBinding));
					memset(soapBinding, 0, sizeof(sdlSoapBinding));
					soapBinding->style = SOAP_DOCUMENT;

					soapBindingNode = get_node_ex(binding->children, "binding", wsdl_soap_namespace);
					if (soapBindingNode) {
						tmp = get_attribute(soapBindingNode->properties, "style");
						if (tmp && !strncmp(tmp->children->content, "rpc", sizeof("rpc"))) {
							soapBinding->style = SOAP_RPC;
						}

						tmp = get_attribute(soapBindingNode->properties, "transport");
						if (tmp) {
							if (strncmp(tmp->children->content, WSDL_HTTP_TRANSPORT, sizeof(WSDL_HTTP_TRANSPORT))) {
								php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: PHP-SOAP doesn't support transport '%s'", tmp->children->content);
							}
							soapBinding->transport = strdup(tmp->children->content);
						}
					}
					tmpbinding->bindingAttributes = (void *)soapBinding;
				}

				name = get_attribute(binding->properties, "name");
				if (name == NULL) {
					php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Missing 'name' attribute for <binding>");
				}
				tmpbinding->name = strdup(name->children->content);

				type = get_attribute(binding->properties, "type");
				if (type == NULL) {
					php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Missing 'type' attribute for <binding>");
				}
				parse_namespace(type->children->content, &ctype, &ns);

				if (zend_hash_find(&ctx.portTypes, ctype, strlen(ctype)+1, (void**)&tmp) != SUCCESS) {
					php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Missing <portType> with name '%s'", name->children->content);
				}
				portType = *tmp;

				if (ctype) {efree(ctype);}
				if (ns) {efree(ns);}

				trav2 = binding->children;
				FOREACHNODE(trav2, "operation", operation) {
					sdlFunctionPtr function;
					xmlNodePtr input, output, fault, portTypeOperation;
					xmlAttrPtr op_name, paramOrder;

					op_name = get_attribute(operation->properties, "name");
					if (op_name == NULL) {
						php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Missing 'name' attribute for <operation>");
					}

					portTypeOperation = get_node_with_attribute(portType->children, "operation", "name", op_name->children->content);
					if (portTypeOperation == NULL) {
						php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Missing <portType>/<operation> with name '%s'", op_name->children->content);
					}

					function = malloc(sizeof(sdlFunction));
					function->functionName = strdup(op_name->children->content);
					function->requestParameters = NULL;
					function->responseParameters = NULL;
					function->responseName = NULL;
					function->requestName = NULL;
					function->bindingAttributes = NULL;

					if (tmpbinding->bindingType == BINDING_SOAP) {
						sdlSoapBindingFunctionPtr soapFunctionBinding;
						sdlSoapBindingPtr soapBinding;
						xmlNodePtr soapOperation;
						xmlAttrPtr tmp;

						soapFunctionBinding = malloc(sizeof(sdlSoapBindingFunction));
						memset(soapFunctionBinding, 0, sizeof(sdlSoapBindingFunction));
						soapBinding = (sdlSoapBindingPtr)tmpbinding->bindingAttributes;
						soapFunctionBinding->style = soapBinding->style;

						soapOperation = get_node_ex(operation->children, "operation", wsdl_soap_namespace);
						if (soapOperation) {
							tmp = get_attribute(soapOperation->properties, "soapAction");
							if (tmp) {
								soapFunctionBinding->soapAction = strdup(tmp->children->content);
							}

							tmp = get_attribute(soapOperation->properties, "style");
							if (tmp) {
								if (!strncmp(tmp->children->content, "rpc", sizeof("rpc"))) {
									soapFunctionBinding->style = SOAP_RPC;
								} else {
									soapFunctionBinding->style = SOAP_DOCUMENT;
								}
							} else {
								soapFunctionBinding->style = soapBinding->style;
							}
						}

						function->bindingAttributes = (void *)soapFunctionBinding;
					}

					input = get_node(portTypeOperation->children, "input");
					if (input != NULL) {
						xmlAttrPtr message, name;

						message = get_attribute(input->properties, "message");
						if (message == NULL) {
							php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Missing name for <input> of '%s'", op_name->children->content);
						}
						function->requestParameters = wsdl_message(&ctx, message->children->content);

						name = get_attribute(input->properties, "name");
						if (name != NULL) {
							function->requestName = strdup(name->children->content);
						} else {
							function->requestName = strdup(function->functionName);
						}

						if (tmpbinding->bindingType == BINDING_SOAP) {
							input = get_node(operation->children, "input");
							if (input != NULL) {
								sdlSoapBindingFunctionPtr soapFunctionBinding = function->bindingAttributes;
								wsdl_soap_binding_body(&ctx, input, wsdl_soap_namespace,&soapFunctionBinding->input);
							}
						}
					}

					output = get_node(portTypeOperation->children, "output");
					if (output != NULL) {
						xmlAttrPtr message, name;

						message = get_attribute(output->properties, "message");
						if (message == NULL) {
							php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Missing name for <output> of '%s'", op_name->children->content);
						}
						function->responseParameters = wsdl_message(&ctx, message->children->content);

						name = get_attribute(output->properties, "name");
						if (name != NULL) {
							function->responseName = strdup(name->children->content);
						} else if (input == NULL) {
							function->responseName = strdup(function->functionName);
						} else {
							function->responseName = malloc(strlen(function->functionName) + sizeof("Response"));
							sprintf(function->responseName, "%sResponse", function->functionName);
						}

						if (tmpbinding->bindingType == BINDING_SOAP) {
							output = get_node(operation->children, "output");
							if (output != NULL) {
								sdlSoapBindingFunctionPtr soapFunctionBinding = function->bindingAttributes;
								wsdl_soap_binding_body(&ctx, output, wsdl_soap_namespace, &soapFunctionBinding->output);
							}
						}
					}

					paramOrder = get_attribute(portTypeOperation->properties, "parameterOrder");
					if (paramOrder) {
						/* FIXME: */
					}

					fault = get_node(operation->children, "fault");
					if (!fault) {
						/* FIXME: */
					}

					function->binding = tmpbinding;

					{
						char *tmp = estrdup(function->functionName);
						int  len = strlen(tmp);

						zend_hash_add(&ctx.root->functions, php_strtolower(tmp, len), len+1, &function, sizeof(sdlFunctionPtr), NULL);
						efree(tmp);
						if (function->requestName != NULL && strcmp(function->requestName,function->functionName) != 0) {
							if (ctx.root->requests == NULL) {
								ctx.root->requests = malloc(sizeof(HashTable));
								zend_hash_init(ctx.root->requests, 0, NULL, NULL, 1);
							}
							tmp = estrdup(function->requestName);
							len = strlen(tmp);
							zend_hash_add(ctx.root->requests, php_strtolower(tmp, len), len+1, &function, sizeof(sdlFunctionPtr), NULL);
							efree(tmp);
						}
					}
				}
				ENDFOREACH(trav2);

				if (!ctx.root->bindings) {
					ctx.root->bindings = malloc(sizeof(HashTable));
					zend_hash_init(ctx.root->bindings, 0, NULL, delete_binding, 1);
				}

				zend_hash_add(ctx.root->bindings, tmpbinding->name, strlen(tmpbinding->name), &tmpbinding, sizeof(sdlBindingPtr), NULL);
			}
			ENDFOREACH(trav);

			zend_hash_move_forward(&ctx.services);
		}
	} else {
		php_error(E_ERROR, "SOAP-ERROR: Parsing WSDL: Couldn't bind to service");
	}

	schema_pass3(ctx.root);
	zend_hash_destroy(&ctx.messages);
	zend_hash_destroy(&ctx.bindings);
	zend_hash_destroy(&ctx.portTypes);
	zend_hash_destroy(&ctx.services);

	return ctx.root;
}

sdlPtr get_sdl(char *uri)
{
	sdlPtr tmp, *hndl;
	TSRMLS_FETCH();

	tmp = NULL;
	hndl = NULL;
	if (zend_hash_find(SOAP_GLOBAL(sdls), uri, strlen(uri), (void **)&hndl) == FAILURE) {
		tmp = load_wsdl(uri);
		zend_hash_add(SOAP_GLOBAL(sdls), uri, strlen(uri), &tmp, sizeof(sdlPtr), NULL);
	} else {
		tmp = *hndl;
	}

	return tmp;
}

/* Deletes */
void delete_sdl(void *handle)
{
	sdlPtr tmp = *((sdlPtr*)handle);

	zend_hash_destroy(&tmp->docs);
	zend_hash_destroy(&tmp->functions);
	if (tmp->source) {
		free(tmp->source);
	}
	if (tmp->target_ns) {
		free(tmp->target_ns);
	}
	if (tmp->encoders) {
		zend_hash_destroy(tmp->encoders);
		free(tmp->encoders);
	}
	if (tmp->types) {
		zend_hash_destroy(tmp->types);
		free(tmp->types);
	}
	if (tmp->elements) {
		zend_hash_destroy(tmp->elements);
		free(tmp->elements);
	}
	if (tmp->attributes) {
		zend_hash_destroy(tmp->attributes);
		free(tmp->attributes);
	}
	if (tmp->attributeGroups) {
		zend_hash_destroy(tmp->attributeGroups);
		free(tmp->attributeGroups);
	}
	if (tmp->groups) {
		zend_hash_destroy(tmp->groups);
		free(tmp->groups);
	}
	if (tmp->bindings) {
		zend_hash_destroy(tmp->bindings);
		free(tmp->bindings);
	}
	if (tmp->requests) {
		zend_hash_destroy(tmp->requests);
		free(tmp->requests);
	}
	free(tmp);
}

static void delete_binding(void *data)
{
	sdlBindingPtr binding = *((sdlBindingPtr*)data);

	if (binding->location) {
		free(binding->location);
	}
	if (binding->name) {
		free(binding->name);
	}

	if (binding->bindingType == BINDING_SOAP) {
		sdlSoapBindingPtr soapBind = binding->bindingAttributes;
		if (soapBind && soapBind->transport) {
			free(soapBind->transport);
		}
	}
}

static void delete_sdl_soap_binding_function_body(sdlSoapBindingFunctionBody body)
{
	if (body.ns) {
		free(body.ns);
	}
	if (body.parts) {
		free(body.parts);
	}
	if (body.encodingStyle) {
		free(body.encodingStyle);
	}
}

static void delete_function(void *data)
{
	sdlFunctionPtr function = *((sdlFunctionPtr*)data);

	if (function->functionName) {
		free(function->functionName);
	}
	if (function->requestName) {
		free(function->requestName);
	}
	if (function->responseName) {
		free(function->responseName);
	}
	if (function->requestParameters) {
		zend_hash_destroy(function->requestParameters);
		free(function->requestParameters);
	}
	if (function->responseParameters) {
		zend_hash_destroy(function->responseParameters);
		free(function->responseParameters);
	}

	if (function->bindingAttributes &&
	    function->binding && function->binding->bindingType == BINDING_SOAP) {
		sdlSoapBindingFunctionPtr soapFunction = function->bindingAttributes;
		if (soapFunction->soapAction) {
			free(soapFunction->soapAction);
		}
		delete_sdl_soap_binding_function_body(soapFunction->input);
		delete_sdl_soap_binding_function_body(soapFunction->output);
		delete_sdl_soap_binding_function_body(soapFunction->falut);
	}
}

static void delete_paramater(void *data)
{
	sdlParamPtr param = *((sdlParamPtr*)data);
	if (param->paramName) {
		free(param->paramName);
	}
	free(param);
}

static void delete_header(void *data)
{
	sdlSoapBindingFunctionHeaderPtr hdr = *((sdlSoapBindingFunctionHeaderPtr*)data);
	if (hdr->name) {
		free(hdr->name);
	}
	if (hdr->ns) {
		free(hdr->ns);
	}
	if (hdr->encodingStyle) {
		free(hdr->encodingStyle);
	}
	free(hdr);
}

static void delete_document(void *doc_ptr)
{
	xmlDocPtr doc = *((xmlDocPtr*)doc_ptr);
	xmlFreeDoc(doc);
}
