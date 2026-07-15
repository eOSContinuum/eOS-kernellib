/*
 * Boot-time test driver for the agent identity substrate.
 *
 * The worked binding of the agent doctrine (docs/identity.md Agent
 * identities): a controller identity is registered through the public
 * authd facade with a foreign-generated WebAuthn vector, mints its own
 * agent (the controller edge derived from the proven session, never
 * supplied), and the agent authenticates through the key ceremony with
 * a foreign-signed, domain-separated assertion (vectors.h; generated
 * by scripts/gen-agent-vectors.py, signatures from the openssl CLI).
 *
 * The boot phases are self-contained on tier-E surfaces: authd for
 * ceremonies and self-service, capabilityd's public is_allowed for the
 * application-action gate. Two things are deliberately NOT reachable
 * from this tier, and that is the doctrine, not a gap: granting the
 * controller a platform capability, and flagging it delegable, are
 * operator actions. The boot phases therefore prove the refusals
 * (delegation without a held capability; the action gate closed), and
 * the operator continuation -- continue_with_operator_grant(), called
 * from the console after `identity grant` + `capability delegable`
 * (scripts/verbsets/agent-app.verbset drives it) -- proves the
 * delegated capability exercised end-to-end, suspension killing it,
 * and resume restoring authentication but never grants.
 *
 * Pass/fail is observable via a sentinel file at
 * /usr/AgentApp/data/test-result.log (the application-tier logging
 * convention shared by the bundled examples). Phases are wrapped in
 * catch{} so a failure in one does not mask another. The driver does
 * not self-exit: it stays up for the operator continuation (timed
 * profile in run-example.sh).
 */

# include <type.h>
# include <kernel/kernel.h>
# include <identityd.h>

inherit "/usr/AgentApp/lib/app";
private inherit hex "/lib/util/hex";

# include "vectors.h"

# define AUTHD		"/usr/System/sys/authd"
# define CAPABILITYD	"/kernel/sys/capabilityd"
# define RESULT_FILE	"/usr/AgentApp/data/test-result.log"
# define APP_CAP	"agentapp.write"

private string csession;	/* the controller's live session */
private string controllerUuid;
private string agentUuid;
private int operatorPhase;	/* the continuation runs once */

private void log_line(string msg)
{
    catch(write_file(RESULT_FILE, msg + "\n"));
}

/*
 * the application action this example gates: a write permitted only
 * to a principal holding APP_CAP -- the application-tier shape of
 * consuming a delegated platform capability
 */
private int app_action(string principal)
{
    return CAPABILITYD->is_allowed(APP_CAP, principal);
}


private void phase_controller()
{
    mixed *res;
    string principal;

    res = AUTHD->register_identity(AA_REG_CHALLENGE,
				   hex::decodeString(AA_REG_CDJ_HEX),
				   hex::decodeString(AA_REG_AO_HEX));
    principal = res[0];
    csession = res[1];
    if (sscanf(principal, "identity:%s", controllerUuid) == 0) {
	log_line("AgentApp:test: FAIL: registration returned " + principal);
	return;
    }
    log_line("AgentApp:test: CONTROLLER-REGISTERED OK");
}

private void phase_mint_agent()
{
    agentUuid = AUTHD->mint_agent(csession, AA_CRED_ID,
				  ([ CRED_TYPE : CRED_TYPE_AGENT_KEY,
				     CRED_SCHEME : "Ed25519",
				     CRED_KEY : hex::decodeString(AA_PUB_HEX),
				     CRED_CREATED : time() ]));
    if (!agentUuid) {
	log_line("AgentApp:test: FAIL: mint_agent returned nil");
	return;
    }
    log_line("AgentApp:test: AGENT-MINTED OK");
}

private void phase_key_ceremony()
{
    mixed *res;

    res = AUTHD->authenticate_agent_key(AA_CH_1, AA_CRED_ID,
					hex::decodeString(AA_SIG_1_HEX));
    if (res[0] != "identity:" + agentUuid) {
	log_line("AgentApp:test: FAIL: ceremony proved " + res[0]);
	return;
    }
    log_line("AgentApp:test: KEY-CEREMONY OK");

    if (AUTHD->validate(res[1]) == "identity:" + agentUuid) {
	log_line("AgentApp:test: AGENT-SESSION OK");
    } else {
	log_line("AgentApp:test: FAIL: agent session does not validate");
    }
}

private void phase_ceremony_negatives()
{
    string err;

    /* the right key over the BARE challenge: the domain tag is
       load-bearing */
    err = catch(AUTHD->authenticate_agent_key(AA_CH_1, AA_CRED_ID,
					      hex::decodeString(AA_SIG_BARE_HEX)));
    if (err == "agentauth: signature invalid") {
	log_line("AgentApp:test: DOMAIN-TAG-REQUIRED OK");
    } else {
	log_line("AgentApp:test: FAIL: bare-challenge signature: " +
		 (err ? err : "accepted"));
    }

    /* a different key over the right message */
    err = catch(AUTHD->authenticate_agent_key(AA_CH_1, AA_CRED_ID,
					      hex::decodeString(AA_SIG_BADKEY_HEX)));
    if (err == "agentauth: signature invalid") {
	log_line("AgentApp:test: WRONG-KEY-REFUSED OK");
    } else {
	log_line("AgentApp:test: FAIL: wrong-key signature: " +
		 (err ? err : "accepted"));
    }
}

private void phase_delegation_refusals()
{
    string err;

    /* the controller holds nothing yet, so delegation refuses on the
       live-hold check -- the operator half has not happened */
    err = catch(AUTHD->delegate_capability(csession, agentUuid, APP_CAP));
    if (err == "identity: delegator does not hold " + APP_CAP) {
	log_line("AgentApp:test: DELEGATE-UNHELD-REFUSED OK");
    } else {
	log_line("AgentApp:test: FAIL: unheld delegation: " +
		 (err ? err : "accepted"));
    }

    if (!app_action("identity:" + agentUuid)) {
	log_line("AgentApp:test: ACTION-GATE-CLOSED OK");
    } else {
	log_line("AgentApp:test: FAIL: action gate open before any grant");
    }
}

private void phase_suspend_resume()
{
    string err;
    mixed *res;

    if (AUTHD->suspend_agent(csession, agentUuid) < 1) {
	log_line("AgentApp:test: FAIL: suspend revoked no sessions");
	return;
    }
    err = catch(AUTHD->authenticate_agent_key(AA_CH_2, AA_CRED_ID,
					      hex::decodeString(AA_SIG_2_HEX)));
    if (err == "agentauth: agent suspended") {
	log_line("AgentApp:test: SUSPEND-BLOCKS-CEREMONY OK");
    } else {
	log_line("AgentApp:test: FAIL: suspended ceremony: " +
		 (err ? err : "accepted"));
    }

    AUTHD->resume_agent(csession, agentUuid);
    res = AUTHD->authenticate_agent_key(AA_CH_2, AA_CRED_ID,
					hex::decodeString(AA_SIG_2_HEX));
    if (res[0] == "identity:" + agentUuid) {
	log_line("AgentApp:test: RESUME-RESTORES-AUTH OK");
    } else {
	log_line("AgentApp:test: FAIL: post-resume ceremony refused");
    }
}

static void run_tests()
{
    string err;

    err = catch(phase_controller());
    if (err) {
	log_line("AgentApp:test: FAIL: controller phase: " + err);
	return;
    }
    err = catch(phase_mint_agent());
    if (err) {
	log_line("AgentApp:test: FAIL: mint phase: " + err);
	return;
    }
    catch(phase_key_ceremony());
    catch(phase_ceremony_negatives());
    catch(phase_delegation_refusals());
    catch(phase_suspend_resume());
    log_line("AgentApp:test: boot phases complete; " +
	     "awaiting the operator continuation");
}

static void create()
{
    /* Defer to a call_out so System/initd has finished its load loop
     * before the driver runs. */
    call_out("setup_and_run", 0);
}

static void setup_and_run()
{
    /* /usr/AgentApp/data/ may not exist on first boot. */
    catch(make_dir("/usr/AgentApp/data"));
    catch(remove_file(RESULT_FILE));
    run_tests();
    /* no self-exit: the operator continuation below still runs */
}


/*
 * the operator-facing reads the continuation verbset needs
 */
string query_controller_uuid()
{
    return controllerUuid;
}

string query_agent_uuid()
{
    return agentUuid;
}

/*
 * the sentinel log, for the continuation verbset (the console's code
 * objects cannot read another domain's files; the driver can read its
 * own)
 */
string query_result_log()
{
    return read_file(RESULT_FILE);
}

/*
 * The operator continuation: called from the console (public on
 * purpose -- it exercises only the driver's own controller session)
 * after the operator has run `identity grant <controller> agentapp.write`
 * and `capability delegable agentapp.write on`. Proves the delegated
 * capability end to end: the controller delegates through authd, the
 * agent's principal passes the application action gate, suspension
 * kills the delegated grant, and resume restores nothing.
 */
string continue_with_operator_grant()
{
    string err, principal;

    if (operatorPhase) {
	return "agent-app: operator phase already ran";
    }
    if (!csession || !agentUuid) {
	return "agent-app: boot phases did not complete";
    }
    operatorPhase = 1;
    principal = "identity:" + agentUuid;

    err = catch(AUTHD->delegate_capability(csession, agentUuid, APP_CAP));
    if (err) {
	log_line("AgentApp:test: FAIL: delegation: " + err);
	return "agent-app: FAIL (see test-result.log)";
    }
    log_line("AgentApp:test: DELEGATED OK");

    if (app_action(principal)) {
	log_line("AgentApp:test: AGENT-EXERCISES-CAP OK");
    } else {
	log_line("AgentApp:test: FAIL: action gate closed after delegation");
    }

    AUTHD->suspend_agent(csession, agentUuid);
    if (!app_action(principal)) {
	log_line("AgentApp:test: SUSPEND-KILLS-DELEGATION OK");
    } else {
	log_line("AgentApp:test: FAIL: delegated grant survived suspension");
    }

    AUTHD->resume_agent(csession, agentUuid);
    if (!app_action(principal)) {
	log_line("AgentApp:test: RESUME-NO-GRANTS OK");
    } else {
	log_line("AgentApp:test: FAIL: resume restored a grant");
    }

    return "agent-app: operator phase complete";
}
