<?php
/**
 * MBC_UserRegistration class - functionality related to the Message Broker
 * consumer mbc-registration-email.
 */
class MBC_UserRegistration
{

  /**
   * Message Broker object that details the connection to RabbitMQ.
   *
   * @var object
   */
  private $MessageBroker;

  /**
   * Details of the channel connection in use by RabbitMQ.
   *
   * @var object
   */
  private $channel;

  /**
   * Collection of configuration settings.
   *
   * @var array
   */
  private $config;

  /**
   * Collection of secret connection settings.
   *
   * @var array
   */
  private $credentials;
  
  /**
   * The number of queue entries to process in each session
   */
  const BATCH_SIZE = 100;

  /**
   * Constructor for MBC_UserRegistration
   *
   * @param array $credentials
   *   Secret settings from mb-secure-config.inc
   *
   * @param array $config
   *   Configuration settings from mb-config.inc
   */
  public function __construct($credentials, $config) {

    $this->config = $config;
    $this->credentials = $credentials;

    // Setup RabbitMQ connection
    $this->MessageBroker = new MessageBroker($credentials, $config);
    $connection = $this->MessageBroker->connection;
    $this->channel = $connection->channel();

    // Exchange
    $this->channel = $this->MessageBroker->setupExchange($config['exchange']['name'], $config['exchange']['type'], $this->channel);

    // Queues - userRegistrationQueue and mailchimpCampaignSignupQueue
    list($this->channel, ) = $this->MessageBroker->setupQueue($config['queue']['registrations']['name'], $this->channel);
    list($this->channel, ) = $this->MessageBroker->setupQueue($config['queue']['campaign_signups']['name'], $this->channel);

    // Queue binding
    $this->channel->queue_bind($config['queue']['registrations']['name'], $config['exchange']['name'], $config['queue']['registrations']['bindingKey']);
    $this->channel->queue_bind($config['queue']['campaign_signups']['name'], $config['exchange']['name'], $config['queue']['campaign_signups']['bindingKey']);

  }

  /**
   * Collect a batch of email address for submission to MailChimp from the
   * related RabbitMQ queue.
   *
   * @return array
   *   An array of the status of the job
   */
  public function consumeNewRegistrationsQueue() {

    // Get the status details of the queue by requesting a declare
    list($this->channel, $status) = $this->MessageBroker->setupQueue($this->config['queue']['registrations']['name'], $this->channel);

    $messageCount = $status[1];
    // @todo: Respond to unacknowledged messages
    $unackedCount = $status[2];

    $messageDetails = '';
    $newSubscribers = array();
    $processedCount = 0;

    while ($messageCount > 0 && $processedCount < self::BATCH_SIZE) {
      $messageDetails = $this->channel->basic_get($this->config['queue']['registrations']['name']);
      $messagePayload = unserialize($messageDetails->body);
      $newSubscribers[] = array(
        'email' => $messagePayload['email'],
        'fname' => $messagePayload['merge_vars']['FNAME'],
        'uid' => $messagePayload['uid'],
        'birthdate' => $messagePayload['birthdate'],
        'mobile' => isset($messagePayload['mobile']) ? $messagePayload['mobile'] : '',
        'mb_delivery_tag' => $messageDetails->delivery_info['delivery_tag'],
      );
      $messageCount--;
      $processedCount++;
    }

    list($composedSubscriberList, $mbDeliveryTags) = $this->composeSubscriberSubmission($newSubscribers);
    if (count($composedSubscriberList) > 0) {
      $results = $this->submitToMailChimp($composedSubscriberList, $mbDeliveryTags);
    }
    else {
      $results = 'No new accounts to submit to MailChimp.';
    }

    return $results;

  }

  /**
   * Collect a batch of email address for submission to MailChimp from the
   * related RabbitMQ queue - assign interest group (campaign).
   *
   * @return array
   *   An array of the status of the job
   */
  public function consumeMailchimpCampaignSignupQueue() {

    // Get the status details of the queue by requesting a declare
    list($this->channel, $status) = $this->MessageBroker->setupQueue($this->config['queue']['campaign_signups']['name'], $this->channel);

    $messageCount = $status[1];
    // @todo: Respond to unacknowledged messages
    $unackedCount = $status[2];

    $messageDetails = '';
    $campaignSignups = array();
    $messagesProcessed = 0;

    while ($messageCount > 0 && $messagesProcessed < self::BATCH_SIZE) {
      $messageDetails = $this->channel->basic_get($this->config['queue']['campaign_signups']['name']);
      $messagePayload = unserialize($messageDetails->body);

      if (isset($messagePayload['mailchimp_group_name']) && $messagePayload['mailchimp_group_name'] != '') {

        $campaignSignups[] = array(
          'email' => $messagePayload['email'],
          'mb_delivery_tag' => $messageDetails->delivery_info['delivery_tag'],
          'mailchimp_group_name' => $messagePayload['mailchimp_group_name'],
          'mailchimp_grouping_id' => $messagePayload['mailchimp_grouping_id'],
        );

      }
      else {
        // No group setting, skip entry in batch submission. Send acknowledgment
        // to remove entry from queue
        $this->channel->basic_ack($messageDetails->delivery_info['delivery_tag']);
        echo $messagePayload['email'] . ' skipped, no group_name.',  "\n";
      }

      $messageCount--;
      $messagesProcessed++;
    }

    list($composedSignupList, $mbDeliveryTags) = $this->composeSignupSubmission($campaignSignups);
    if (count($composedSignupList)) {
      $results = $this->submitToMailChimp($composedSignupList, $mbDeliveryTags);
    }
    else {
      $results = 'No new campaign signups accounts to submit to MailChimp.';
    }

    return $results;
  }

  /**
   * Format email list to meet MailChimp API requirements for batchSubscribe
   *
   * @param array $newSubscribers
   *   The list of email address to be formatted
   *
   * @return array
   *   Array of email addresses formatted to meet MailChimp API requirements.
   */
  private function composeSubscriberSubmission($newSubscribers = array()) {

    $composedSubscriberList = array();
    $mbDeliveryTags = array();

    $composedSubscriberList = array();
    foreach ($newSubscribers as $newSubscriber) {
      // Dont add address of users under 13
      if ($newSubscriber['birthdate'] < time() - (60 * 60 * 24 * 365 * 13)) {
        $composedSubscriberList[] = array(
          'email' => array(
            'email' => $newSubscriber['email']
          ),
          'merge_vars' => array(
              'UID' => $newSubscriber['uid'],
              'MMERGE3' => $newSubscriber['fname'],
              'BDAY' => date('m/d', $newSubscriber['birthdate']),
              'BDAYFULL' => date('m/d/Y', $newSubscriber['birthdate']),
              'MMERGE7' => $newSubscriber['mobile'],
          ),
        );
        $mbDeliveryTags[$newSubscriber['email']] = $newSubscriber['mb_delivery_tag'];
      }
    }

    return array($composedSubscriberList, $mbDeliveryTags);
  }

  /**
   * Format email list to meet MailChimp API requirements for batchSubscribe
   *
   * @param array $campaignSignups
   *   The list of email address to be formatted
   *
   * @return array
   *   Array of email addresses formatted to meet MailChimp API requirements.
   */
  private function composeSignupSubmission($campaignSignups = array()) {

    $composedSubscriberList = array();
    $mbDeliveryTags = array();

    foreach ($campaignSignups as $campaignSignup) {
      $composedSubscriberList[] = array(
        'email' => array(
          'email' => $campaignSignup['email']
        ),
        'merge_vars' => array(
          'groupings' => array(
            0 => array(
              'id' => $campaignSignup['mailchimp_grouping_id'], // Campaigns2013 (10621) or Campaigns2014 (10637)
              'groups' => array($campaignSignup['mailchimp_group_name']),
            )
          ),
        ),
      );
      $mbDeliveryTags[$campaignSignup['email']] = $campaignSignup['mb_delivery_tag'];
    }

    return array($composedSubscriberList, $mbDeliveryTags);
  }

  /**
   * Make signup submission to MailChimp
   *
   * @param array $composedBatch
   *   The list of email address to be submitted to MailChimp
   *
   * @param array $mbDeliveryTags
   *   A list of RabbitMQ delivery tags being processed in batch.
   *
   * @return array
   *   A list of the RabbitMQ queue entry IDs that have been successfully
   *   submitted to MailChimp.
   */
  private function submitToMailChimp($composedBatch = array(), $mbDeliveryTags = array()) {

    $MailChimp = new \Drewm\MailChimp($this->credentials['mailchimp_apikey']);

    // $results1 = $MailChimp->call("lists/list", array());
    // $results2 = $MailChimp->call("lists/interest-groupings", array('id' => 'f2fab1dfd4'));

    // batchSubscribe($id, $batch, $double_optin=true, $update_existing=false, $replace_interests=true)
    // replace_interests: optional - flag to determine whether we replace the
    // interest groups with the updated groups provided, or we add the provided
    // groups to the member's interest groups (optional, defaults to true)
        // Lookup list details including "mailchimp_list_id"
    // -> 71893 "Do Something Members" is f2fab1dfd4 (who knows why?!?)

    $acknowledgeOK = TRUE;
    $results = $MailChimp->call("lists/batch-subscribe", array(
      'id' => $this->config['mailchimp_list_id'],
      'batch' => $composedBatch,
      'double_optin' => FALSE,
      'update_existing' => TRUE,
      'replace_interests' => FALSE
    ));

    $status = '------- ' . date('D M j G:i:s:u T Y') . ' -------' . "\n";
    $results['add_count'] = 0;
    $results['update_count'] = 0;
    if ($results['error_count'] > 0) {
      $status .= ' [x] ' . $results['add_count'] . ' email addresses added / ' . $results['update_count'] . ' updated.' . "\n";
      foreach ($results['errors'] as $error){
        if ($error['code'] == 212 || $error['code'] == 220 || $error['code'] == 213) { // unsubscribed, banned, bounced
          $status .= "**************\n";
          $status .= $error['error'] . "\n";
        }
        elseif ($error['code'] == -99) {  // fake
          $status .= "%%%%%%%%%%%%%%\n";
          $status .= $error['error'] . "\n";
          unset($mbDeliveryTags[$error['email']['email']]);
        }
        else {
          $status .= $results['error_count'] . ' errors reported, batch failed!' . "\n";
	        $status .= 'errors: '. print_r($results['errors'], TRUE) . "\n";
          $acknowledgeOK = FALSE;
        }
      }

    }
    else {
      $status .= 'mbc-registration-email - submitToMailChimp(): Success!' . "\n";
      $status .= ' [x] ' . $results['add_count'] . ' email addresses added / ' . $results['update_count'] . ' updated.' . "\n";
    }

    // Acknowledge submissions have been processed by MailChimp
    if ($acknowledgeOK) {
      foreach($mbDeliveryTags as $tagEmail => $mbDeliveryTag) {
        $this->channel->basic_ack($mbDeliveryTag);
      }
    }

    return $status;

  }

}