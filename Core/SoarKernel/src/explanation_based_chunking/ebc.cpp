/*
 * variablization_manager.cpp
 *
 *  Created on: Jul 25, 2013
 *      Author: mazzin
 */

#include "ebc.h"
#include "ebc_settings.h"
#include "ebc_timers.h"

#include "agent.h"
#include "condition.h"
#include "decide.h"
#include "dprint.h"
#include "explanation_memory.h"
#include "instantiation.h"
#include "output_manager.h"
#include "preference.h"
#include "production.h"
#include "print.h"
#include "rhs.h"
#include "rhs_functions.h"
#include "soar_instance.h"
#include "soar_TraceNames.h"
#include "test.h"
#include "working_memory.h"
#include "xml.h"

#include <assert.h>

using namespace soar_TraceNames;

extern Symbol* find_goal_at_goal_stack_level(agent* thisAgent, goal_stack_level level);
extern Symbol* find_impasse_wme_value(Symbol* id, Symbol* attr);
byte type_of_existing_impasse(agent* thisAgent, Symbol* goal);

Explanation_Based_Chunker::Explanation_Based_Chunker(agent* myAgent)
{
    /* Cache agent and Output Manager pointer */
    thisAgent = myAgent;
    outputManager = &Output_Manager::Get_OM();

    /* Create the parameter object where the cli settings are stored.
     * This also initializes the ebc_settings array */
    ebc_params = new ebc_param_container(thisAgent, ebc_settings, max_chunks, max_dupes);

    /* Create the timers */
    ebc_timers = new ebc_timer_container(thisAgent);

    /* Create data structures used for EBC */
    instantiation_identities = new sym_to_id_map();
    constraints = new constraint_list();
    inst_id_to_identity_map = new id_to_join_map();
    cond_merge_map = new triple_merge_map();
    local_linked_STIs = new rhs_value_list();
    m_sym_to_var_map = new sym_to_sym_id_map();

    init_chunk_cond_set(&negated_set);

    /* Initialize learning setting */
    chunk_name_prefix = make_memory_block_for_string(thisAgent, "chunk");
    justification_name_prefix = make_memory_block_for_string(thisAgent, "justify");

    singletons = new symbol_set();

    lti_link_function = NULL;
    reinit();
}

Explanation_Based_Chunker::~Explanation_Based_Chunker()
{
    clear_data();

    delete ebc_params;
    delete ebc_timers;

    delete instantiation_identities;
    delete constraints;
    delete inst_id_to_identity_map;
    delete cond_merge_map;
    delete local_linked_STIs;
    delete m_sym_to_var_map;
    free_memory_block_for_string(thisAgent, chunk_name_prefix);
    free_memory_block_for_string(thisAgent, justification_name_prefix);
    if (ebc_settings[SETTING_EBC_LEARNING_ON]) clear_singletons();
    delete singletons;
}

void Explanation_Based_Chunker::reinit()
{
    dprint(DT_VARIABLIZATION_MANAGER, "Original_Variable_Manager reinitializing...\n");
    clear_data();
    ebc_timers->reset();
    inst_id_counter                     = 0;
    prod_id_counter                     = 0;
    identity_counter                = 0;
    inst_identity_counter     = 0;
    backtrace_number                    = 0;
    chunk_naming_counter                = 0;
    justification_naming_counter        = 0;
    grounds_tc                          = 0;
    m_results_match_goal_level          = 0;
    m_goal_level                        = 0;
    m_results_tc                        = 0;
    m_correctness_issue_possible        = true;
    m_inst                              = NULL;
    m_results                           = NULL;
    m_extra_results                     = NULL;
    m_lhs                               = NULL;
    m_rhs                               = NULL;
    m_prod                              = NULL;
    m_chunk_inst                        = NULL;
    m_prod_name                         = NULL;
    chunk_free_problem_spaces           = NIL;
    chunky_problem_spaces               = NIL;
    m_failure_type                      = ebc_success;
    m_rule_type                         = ebc_no_rule;
    m_learning_on_for_instantiation     = ebc_settings[SETTING_EBC_LEARNING_ON];
}

bool Explanation_Based_Chunker::set_learning_for_instantiation(instantiation* inst)
{
    if (!ebc_settings[SETTING_EBC_LEARNING_ON] || (inst->match_goal_level == TOP_GOAL_LEVEL))
    {
        m_learning_on_for_instantiation = false;
        return false;
    }

    if (ebc_settings[SETTING_EBC_EXCEPT] && member_of_list(inst->match_goal, chunk_free_problem_spaces))
    {
        if (thisAgent->trace_settings[TRACE_CHUNKS_WARNINGS_SYSPARAM])
        {
            std::ostringstream message;
            message << "\nWill not attempt to learn a chunk for match of " << inst->prod_name->to_string() << " because state " << inst->match_goal->to_string() << " was flagged to prevent learning";
            thisAgent->outputManager->printa_sf(thisAgent,  message.str().c_str());
            xml_generate_verbose(thisAgent, message.str().c_str());

        }
        m_learning_on_for_instantiation = false;
        return false;
    }

    if (ebc_settings[SETTING_EBC_ONLY]  && !member_of_list(inst->match_goal, chunky_problem_spaces))
    {
        if (thisAgent->trace_settings[TRACE_CHUNKS_WARNINGS_SYSPARAM])
        {
            std::ostringstream message;
            message << "\nWill not attempt to learn a chunk for match of " << inst->prod_name->to_string() << " because state " << inst->match_goal->to_string() << " was not flagged for learning";
            thisAgent->outputManager->printa_sf(thisAgent,  message.str().c_str());
            xml_generate_verbose(thisAgent, message.str().c_str());
        }
        m_learning_on_for_instantiation = false;
        return false;
    }

    /* Allow_bottom_up_chunks will be false if a chunk was already learned in a lower goal */
    if (ebc_settings[SETTING_EBC_BOTTOM_ONLY]  &&
            !inst->match_goal->id->allow_bottom_up_chunks)
    {
        if (thisAgent->trace_settings[TRACE_CHUNKS_WARNINGS_SYSPARAM])
        {
            std::ostringstream message;
            message << "\nWill not attempt to learn a chunk for match of " << inst->prod_name->to_string() << " because state " << inst->match_goal->to_string() << " is not the bottom state";
            thisAgent->outputManager->printa_sf(thisAgent,  message.str().c_str());
            xml_generate_verbose(thisAgent, message.str().c_str());
        }
        m_learning_on_for_instantiation = false;
        return false;
    }

    m_learning_on_for_instantiation = true;
    return true;
}

Symbol* Explanation_Based_Chunker::generate_name_for_new_rule()
{
    uint64_t        rule_number;
    uint64_t*       rule_naming_counter;
    char*           rule_prefix;
    std::string     new_rule_name;

    /* Step 1: Get prefix and rule count */
    if (m_rule_type == ebc_chunk)
    {
        rule_prefix = chunk_name_prefix;
        rule_number = chunks_this_d_cycle;
        rule_naming_counter = &chunk_naming_counter;
    } else {
        rule_prefix = justification_name_prefix;
        rule_number = justifications_this_d_cycle;
        rule_naming_counter = &justification_naming_counter;
    }

    if (!ebc_settings[SETTING_EBC_LEARNING_ON] || (ebc_params->naming_style->get_value() == numberedFormat))
    {
        increment_counter((*rule_naming_counter));
        return thisAgent->symbolManager->generate_new_str_constant(rule_prefix, rule_naming_counter);
    }

    new_rule_name += rule_prefix;

    /* Step 2: Add learning depth to indicate learned rule based on another learned rule */
    if (m_inst->prod)
    {
        m_chunk_inst->prod_naming_depth = m_inst->prod_naming_depth + 1;
        if (m_inst->prod_naming_depth)
        {
            new_rule_name += 'x';
            new_rule_name += std::to_string(m_chunk_inst->prod_naming_depth);
        }
        new_rule_name += '*';
        new_rule_name +=  m_inst->prod->original_rule_name;
    }

    /* Step 3: Add impasse type */
    switch (m_inst->match_goal->id->higher_goal->id->impasse_type)
    {
        case CONSTRAINT_FAILURE_IMPASSE_TYPE:
            new_rule_name += "*Failure";
            break;
        case CONFLICT_IMPASSE_TYPE:
            new_rule_name += "*Conflict";
            break;
        case TIE_IMPASSE_TYPE:
            new_rule_name += "*Tie";
            break;
        case ONC_IMPASSE_TYPE:
            new_rule_name += "*OpNoChange";
            break;
        case SNC_IMPASSE_TYPE:
            new_rule_name += "*StateNoChange";
            break;
        default:
            break;
    }

    /* Step 4:  Add time */
    new_rule_name += "*t";
    if (thisAgent->init_count)
    {
        new_rule_name += std::to_string(thisAgent->init_count + 1);
        new_rule_name += '-';
    }
    new_rule_name += std::to_string(thisAgent->d_cycle_count);
    new_rule_name += '-';
    new_rule_name += std::to_string(rule_number);

    if (thisAgent->symbolManager->find_str_constant(new_rule_name.c_str()))
    {
        uint64_t dummy_counter = 2;
        return thisAgent->symbolManager->generate_new_str_constant(new_rule_name.c_str(), &dummy_counter);
    }
    else
        return thisAgent->symbolManager->make_str_constant_no_find(new_rule_name.c_str());
}

void Explanation_Based_Chunker::set_up_rule_name()
{
    /* Generate a new symbol for the name of the new chunk or justification */
    if (m_rule_type == ebc_chunk)
    {
        chunks_this_d_cycle++;
        m_prod_name = generate_name_for_new_rule();

        m_prod_type = CHUNK_PRODUCTION_TYPE;
        m_should_print_name = (thisAgent->trace_settings[TRACE_CHUNK_NAMES_SYSPARAM] != 0);
        m_should_print_prod = (thisAgent->trace_settings[TRACE_CHUNKS_SYSPARAM] != 0);
    }
    else
    {
        justifications_this_d_cycle++;
        m_prod_name = generate_name_for_new_rule();
        m_prod_type = JUSTIFICATION_PRODUCTION_TYPE;
        m_should_print_name = (thisAgent->trace_settings[TRACE_JUSTIFICATION_NAMES_SYSPARAM] != 0);
        m_should_print_prod = (thisAgent->trace_settings[TRACE_JUSTIFICATIONS_SYSPARAM] != 0);
        #ifdef EBC_DEBUG_STATISTICS
            thisAgent->explanationMemory->increment_stat_justifications_attempted();
        #endif
    }

    if (m_should_print_name)
    {
        thisAgent->outputManager->start_fresh_line(thisAgent);
        thisAgent->outputManager->printa_sf(thisAgent, "\nLearning new rule %y\n", m_prod_name);
        xml_begin_tag(thisAgent, kTagLearning);
        xml_begin_tag(thisAgent, kTagProduction);
        xml_att_val(thisAgent, kProduction_Name, m_prod_name);
        xml_end_tag(thisAgent, kTagProduction);
        xml_end_tag(thisAgent, kTagLearning);
    }
}
void Explanation_Based_Chunker::clear_data()
{
    if (ebc_settings[SETTING_EBC_LEARNING_ON])
    {
        dprint(DT_VARIABLIZATION_MANAGER, "Clearing all EBC maps.\n");
        clear_cached_constraints();
        clean_up_identities();
        clear_merge_map();
        clear_symbol_identity_map();
        clear_id_to_identity_map();
    }
}

void Explanation_Based_Chunker::sanity_chunk_test (test pTest)
{
    if (pTest->type == CONJUNCTIVE_TEST)
    {
        for (cons* c = pTest->data.conjunct_list; c != NIL; c = c->rest)
        {
            sanity_chunk_test(static_cast<test>(c->first));
        }
    } else {
        assert((!test_has_referent(pTest) || !pTest->data.referent->is_sti()) && !pTest->identity);
    }
}

void Explanation_Based_Chunker::sanity_chunk_conditions(condition* top_cond)
{
    for (condition* cond = top_cond; cond != NIL; cond = cond->next)
    {
        if (cond->type != CONJUNCTIVE_NEGATION_CONDITION)
        {
            sanity_chunk_test(cond->data.tests.id_test);
            sanity_chunk_test(cond->data.tests.attr_test);
            sanity_chunk_test(cond->data.tests.value_test);
        }
        else
        {
            sanity_chunk_conditions(cond->data.ncc.top);
        }
    }
}

void Explanation_Based_Chunker::sanity_justification_test (test pTest, bool pIsNCC)
{
    if (pTest->type == CONJUNCTIVE_TEST)
    {
        for (cons* c = pTest->data.conjunct_list; c != NIL; c = c->rest)
        {
            sanity_justification_test(static_cast<test>(c->first), pIsNCC);
        }
    } else {
        if (pIsNCC)
        {
            assert(!test_has_referent(pTest) || (!pTest->data.referent->is_variable() || !pTest->identity));

        } else {
            assert(!test_has_referent(pTest) || !pTest->data.referent->is_variable() || !pTest->identity);
        }
    }
}

goal_stack_level Explanation_Based_Chunker::get_inst_match_level()
{
    if (m_inst)
        return m_inst->match_goal_level;
    else return 0;
}

ebc_timer_container::ebc_timer_container(agent* new_agent): soar_module::timer_container(new_agent)
{
    instantiation_creation = new ebc_timer("1.00 Instantiation creation", thisAgent, soar_module::timer::one);
    chunk_instantiation_creation = new ebc_timer("2.01 Chunk instantiation creation", thisAgent, soar_module::timer::one);
    dependency_analysis = new ebc_timer("2.02 Dependency analysis", thisAgent, soar_module::timer::one);
    identity_unification = new ebc_timer("2.03 Identity unification", thisAgent, soar_module::timer::one);
    identity_update = new ebc_timer("2.04 Identity transitive updates", thisAgent, soar_module::timer::one);
    variablization_lhs = new ebc_timer("2.05 Variablizing LHS", thisAgent, soar_module::timer::one);
    variablization_rhs = new ebc_timer("2.06 Variablizing RHS", thisAgent, soar_module::timer::one);
    merging = new ebc_timer("2.07 Merging Conditions", thisAgent, soar_module::timer::one);
    reorder = new ebc_timer("2.08 Validation and reordering", thisAgent, soar_module::timer::one);
    repair = new ebc_timer("2.09 Rule repair", thisAgent, soar_module::timer::one);
    reinstantiate = new ebc_timer("2.10 Reinstantiation", thisAgent, soar_module::timer::one);
    add_to_rete = new ebc_timer("2.11 Adding rule to RETE", thisAgent, soar_module::timer::one);
    clean_up = new ebc_timer("2.12 EBC Clean-Up", thisAgent, soar_module::timer::one);
    ebc_total = new ebc_timer("2.13 EBC Total", thisAgent, soar_module::timer::one);

    add(instantiation_creation);
    add(ebc_total);
    add(dependency_analysis);
    add(chunk_instantiation_creation);
    add(variablization_lhs);
    add(variablization_rhs);
    add(merging);
    add(repair);
    add(reorder);
    add(reinstantiate);
    add(add_to_rete);
    add(clean_up);
    add(identity_unification);
    add(identity_update);
}

ebc_timer_level_predicate::ebc_timer_level_predicate(agent* new_agent): soar_module::agent_predicate<soar_module::timer::timer_level>(new_agent) {}

bool ebc_timer_level_predicate::operator()(soar_module::timer::timer_level val)
{
    return (thisAgent->explanationBasedChunker->ebc_params->timers_cmd->get_value() == on);
}

ebc_timer::ebc_timer(const char* new_name, agent* new_agent, soar_module::timer::timer_level new_level): soar_module::timer(new_name, new_agent, new_level, new ebc_timer_level_predicate(new_agent)) {}

